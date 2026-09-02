#include "include/webview_windows/webview_windows_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "util/string_converter.h"
#include "webview_bridge.h"
#include "webview_host.h"
#include "webview_platform.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

namespace {

constexpr auto kMethodInitialize = "initialize";
constexpr auto kMethodDispose = "dispose";
constexpr auto kMethodInitializeEnvironment = "initializeEnvironment";
constexpr auto kMethodGetWebViewVersion = "getWebViewVersion";

constexpr auto kErrorCodeInvalidId = "invalid_id";
constexpr auto kErrorCodeEnvironmentCreationFailed =
    "environment_creation_failed";
constexpr auto kErrorCodeEnvironmentAlreadyInitialized =
    "environment_already_initialized";
constexpr auto kErrorCodeWebviewCreationFailed = "webview_creation_failed";
constexpr auto kErrorUnsupportedPlatform = "unsupported_platform";

template <typename T>
std::optional<T> GetOptionalValue(const flutter::EncodableMap& map,
                                  const std::string& key) {
  const auto it = map.find(flutter::EncodableValue(key));
  if (it != map.end()) {
    const auto val = std::get_if<T>(&it->second);
    if (val) {
      return *val;
    }
  }
  return std::nullopt;
}

class WebviewWindowsPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  WebviewWindowsPlugin(flutter::TextureRegistrar* textures,
                       flutter::BinaryMessenger* messenger);

  virtual ~WebviewWindowsPlugin();

 private:
  std::unique_ptr<WebviewPlatform> platform_;

  // MULTI-ENVIRONMENT SUPPORT: previously a single `webview_host_` was
  // shared by the whole process, so only ONE WebView2 environment
  // (== one userDataPath == effectively one account) could be alive at
  // a time; switching accounts required fully disposing the old
  // environment before the new one could be created. Keyed by an
  // explicit `environmentId` string from Dart (empty string "" is the
  // legacy/default environment, kept for backward compatibility with
  // callers that never pass one), this lets several environments —
  // e.g. one per account — live side by side in the same process.
  std::unordered_map<std::string, std::shared_ptr<WebviewHost>>
      webview_hosts_;
  std::unordered_map<int64_t, std::unique_ptr<WebviewBridge>> instances_;
  // Tracks which environmentId each texture_id/webview instance belongs
  // to, so we know when an environment's last instance is gone and it's
  // safe to erase that specific `WebviewHost` (without touching other
  // environments' hosts).
  std::unordered_map<int64_t, std::string> instance_environment_ids_;

  WNDCLASS window_class_ = {};
  flutter::TextureRegistrar* textures_;
  flutter::BinaryMessenger* messenger_;

  bool InitPlatform();

  // Returns the "environmentId" argument if present in `args` (nullable),
  // or the empty string (legacy/default environment) otherwise.
  static std::string GetEnvironmentId(const flutter::EncodableValue* args);

  void CreateWebviewInstance(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>);
  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

// static
void WebviewWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "io.jns.webview.win",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<WebviewWindowsPlugin>(
      registrar->texture_registrar(), registrar->messenger());

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

WebviewWindowsPlugin::WebviewWindowsPlugin(flutter::TextureRegistrar* textures,
                                           flutter::BinaryMessenger* messenger)
    : textures_(textures), messenger_(messenger) {
  window_class_.lpszClassName = L"FlutterWebviewMessage";
  window_class_.lpfnWndProc = &DefWindowProc;
  RegisterClass(&window_class_);
}

WebviewWindowsPlugin::~WebviewWindowsPlugin() {
  instances_.clear();
  UnregisterClass(window_class_.lpszClassName, nullptr);
}

// static
std::string WebviewWindowsPlugin::GetEnvironmentId(
    const flutter::EncodableValue* args) {
  if (args) {
    if (const auto* map = std::get_if<flutter::EncodableMap>(args)) {
      auto id = GetOptionalValue<std::string>(*map, "environmentId");
      if (id.has_value()) {
        return *id;
      }
    }
  }
  return std::string();  // legacy/default shared environment
}

void WebviewWindowsPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare(kMethodInitializeEnvironment) == 0) {
    const auto environment_id = GetEnvironmentId(method_call.arguments());

    if (webview_hosts_.find(environment_id) != webview_hosts_.end()) {
      return result->Error(kErrorCodeEnvironmentAlreadyInitialized,
                           "The webview environment is already initialized"
                           " for this environmentId");
    }

    if (!InitPlatform()) {
      return result->Error(kErrorUnsupportedPlatform,
                           "The platform is not supported");
    }

    const auto& map = std::get<flutter::EncodableMap>(*method_call.arguments());

    std::optional<std::wstring> browser_exe_wpath = std::nullopt;
    std::optional<std::string> browser_exe_path =
        GetOptionalValue<std::string>(map, "browserExePath");
    if (browser_exe_path) {
      browser_exe_wpath = util::Utf16FromUtf8(*browser_exe_path);
    }

    std::optional<std::wstring> user_data_wpath = std::nullopt;
    std::optional<std::string> user_data_path =
        GetOptionalValue<std::string>(map, "userDataPath");
    if (user_data_path) {
      user_data_wpath = util::Utf16FromUtf8(*user_data_path);
    } else {
      user_data_wpath = platform_->GetDefaultDataDirectory();
    }

    std::optional<std::string> additional_args =
        GetOptionalValue<std::string>(map, "additionalArguments");

    // Each environmentId gets its OWN ICoreWebView2Environment (own
    // userDataPath/profile), so several accounts can each be genuinely
    // isolated AND alive at the same time — not just one-at-a-time.
    std::shared_ptr<WebviewHost> host(std::move(WebviewHost::Create(
        platform_.get(), user_data_wpath, browser_exe_wpath, additional_args)));
    if (!host) {
      return result->Error(kErrorCodeEnvironmentCreationFailed);
    }
    webview_hosts_[environment_id] = std::move(host);

    return result->Success();
  }

  if (method_call.method_name().compare(kMethodGetWebViewVersion) == 0) {
    LPWSTR version_info = nullptr;
    auto hr =
        GetAvailableCoreWebView2BrowserVersionString(nullptr, &version_info);
    if (SUCCEEDED(hr) && version_info != nullptr) {
      return result->Success(
          flutter::EncodableValue(util::Utf8FromUtf16(version_info)));
    } else {
      return result->Success();
    }
  }

  if (method_call.method_name().compare(kMethodInitialize) == 0) {
    return CreateWebviewInstance(method_call.arguments(), std::move(result));
  }

  if (method_call.method_name().compare(kMethodDispose) == 0) {
    if (const auto texture_id = std::get_if<int64_t>(method_call.arguments())) {
      const auto it = instances_.find(*texture_id);
      if (it != instances_.end()) {
        instances_.erase(it);

        // Only release the environment THIS instance belonged to, once
        // no other instance is still using it — other accounts'
        // environments/hosts are untouched, so they stay warm
        // (concurrently alive) instead of being forced to reload.
        std::string environment_id;
        const auto env_it = instance_environment_ids_.find(*texture_id);
        if (env_it != instance_environment_ids_.end()) {
          environment_id = env_it->second;
          instance_environment_ids_.erase(env_it);
        }

        bool environment_still_in_use = false;
        for (const auto& kv : instance_environment_ids_) {
          if (kv.second == environment_id) {
            environment_still_in_use = true;
            break;
          }
        }
        if (!environment_still_in_use) {
          webview_hosts_.erase(environment_id);
        }

        return result->Success();
      }
    }
    return result->Error(kErrorCodeInvalidId);
  } else {
    result->NotImplemented();
  }
}

void WebviewWindowsPlugin::CreateWebviewInstance(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (!InitPlatform()) {
    return result->Error(kErrorUnsupportedPlatform,
                         "The platform is not supported");
  }

  const auto environment_id = GetEnvironmentId(arguments);

  std::shared_ptr<WebviewHost> host;
  const auto host_it = webview_hosts_.find(environment_id);
  if (host_it != webview_hosts_.end()) {
    host = host_it->second;
  } else if (environment_id.empty()) {
    // Legacy behavior preserved: the default ("") environment is
    // lazily created on first use if initializeEnvironment() was never
    // called explicitly.
    host = std::shared_ptr<WebviewHost>(std::move(WebviewHost::Create(
        platform_.get(), platform_->GetDefaultDataDirectory())));
    if (!host) {
      return result->Error(kErrorCodeEnvironmentCreationFailed);
    }
    webview_hosts_[environment_id] = host;
  } else {
    // A non-default environmentId must be explicitly initialized first
    // (it carries a specific userDataPath the caller chose deliberately
    // — silently falling back to a shared default here is exactly the
    // "two accounts silently sharing one profile" bug this design is
    // meant to avoid).
    return result->Error(
        kErrorCodeInvalidId,
        "No environment initialized for this environmentId. Call "
        "initializeEnvironment(environmentId: ...) first.");
  }

  auto hwnd =
      CreateWindowEx(0, window_class_.lpszClassName, L"", 0, 0, 0, 0, 0,
                     HWND_MESSAGE, nullptr, window_class_.hInstance, nullptr);

  std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
      shared_result = std::move(result);
  host->CreateWebview(
      hwnd, true, true,
      [shared_result, this, environment_id](
          std::unique_ptr<Webview> webview,
          std::unique_ptr<WebviewCreationError> error) {
        if (!webview) {
          if (error) {
            return shared_result->Error(
                kErrorCodeWebviewCreationFailed,
                std::format(
                    "Creating the webview failed: {} (HRESULT: {:#010x})",
                    error->message, error->hr));
          }
          return shared_result->Error(kErrorCodeWebviewCreationFailed,
                                      "Creating the webview failed.");
        }

        auto bridge = std::make_unique<WebviewBridge>(
            messenger_, textures_, platform_->graphics_context(),
            std::move(webview));
        auto texture_id = bridge->texture_id();
        instances_[texture_id] = std::move(bridge);
        instance_environment_ids_[texture_id] = environment_id;

        auto response = flutter::EncodableValue(flutter::EncodableMap{
            {flutter::EncodableValue("textureId"),
             flutter::EncodableValue(texture_id)},
        });

        shared_result->Success(response);
      });
}

bool WebviewWindowsPlugin::InitPlatform() {
  if (!platform_) {
    platform_ = std::make_unique<WebviewPlatform>();
  }
  return platform_->IsSupported();
}

}  // namespace

void WebviewWindowsPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  WebviewWindowsPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}