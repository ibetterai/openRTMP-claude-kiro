// OpenRTMP - Cross-platform RTMP Server
// Windows Service Wrapper Implementation
//
// Requirements Covered: 7.4, 7.5, 7.6
// - 7.4: Support binding to any available network interface
// - 7.5: Support running as a background service or daemon
// - 7.6: Windows Firewall integration and x64/ARM64 support

#include "openrtmp/pal/windows/windows_service_wrapper.hpp"

#include <chrono>
#include <sstream>
#include <thread>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsvc.h>
#include <netfw.h>
#include <comdef.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#endif // _WIN32

namespace openrtmp {
namespace pal {
namespace windows {

// =============================================================================
// Static Members
// =============================================================================

#if defined(_WIN32)
WindowsServiceWrapper* WindowsServiceWrapper::instance_ = nullptr;
std::mutex WindowsServiceWrapper::instanceMutex_;
#endif

// =============================================================================
// Constructor / Destructor
// =============================================================================

WindowsServiceWrapper::WindowsServiceWrapper()
    : shutdownRequested_(false)
    , shutdownReason_(ShutdownReason::Unknown)
    , currentState_(ServiceState::Stopped)
    , gracePeriod_(std::chrono::seconds(30))
    , cachedArchitecture_(ProcessorArchitecture::Unknown)
    , architectureDetected_(false)
#if defined(_WIN32)
    , serviceStatusHandle_(nullptr)
    , eventLogHandle_(nullptr)
#endif
{
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ == nullptr) {
        instance_ = this;
    }
#endif
}

WindowsServiceWrapper::~WindowsServiceWrapper() {
#if defined(_WIN32)
    if (eventLogHandle_ != nullptr) {
        DeregisterEventSource(static_cast<HANDLE>(eventLogHandle_));
        eventLogHandle_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ == this) {
        instance_ = nullptr;
    }
#endif
}

// =============================================================================
// Architecture Detection (Requirement 7.6)
// =============================================================================

ProcessorArchitecture WindowsServiceWrapper::detectArchitecture() const {
#if defined(_WIN32)
    SYSTEM_INFO sysInfo;
    GetNativeSystemInfo(&sysInfo);

    switch (sysInfo.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            return ProcessorArchitecture::x64;
        case PROCESSOR_ARCHITECTURE_ARM64:
            return ProcessorArchitecture::ARM64;
        case PROCESSOR_ARCHITECTURE_INTEL:
            return ProcessorArchitecture::x86;
        case PROCESSOR_ARCHITECTURE_ARM:
            return ProcessorArchitecture::ARM;
        default:
            return ProcessorArchitecture::Unknown;
    }
#else
    // Non-Windows platforms: detect based on compile-time architecture
#if defined(__x86_64__) || defined(_M_X64)
    return ProcessorArchitecture::x64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return ProcessorArchitecture::ARM64;
#elif defined(__i386__) || defined(_M_IX86)
    return ProcessorArchitecture::x86;
#elif defined(__arm__) || defined(_M_ARM)
    return ProcessorArchitecture::ARM;
#else
    return ProcessorArchitecture::Unknown;
#endif
#endif
}

ProcessorArchitecture WindowsServiceWrapper::getProcessorArchitecture() const {
    if (!architectureDetected_) {
        cachedArchitecture_ = detectArchitecture();
        architectureDetected_ = true;
    }
    return cachedArchitecture_;
}

std::string WindowsServiceWrapper::getProcessorArchitectureString() const {
    ProcessorArchitecture arch = getProcessorArchitecture();

    switch (arch) {
        case ProcessorArchitecture::x86:
            return "x86";
        case ProcessorArchitecture::x64:
            return "x64";
        case ProcessorArchitecture::ARM:
            return "ARM";
        case ProcessorArchitecture::ARM64:
            return "ARM64";
        default:
            return "Unknown";
    }
}

bool WindowsServiceWrapper::is64BitProcess() const {
#if defined(_WIN32)
#if defined(_WIN64)
    return true;
#else
    // Check if running in WOW64
    BOOL isWow64 = FALSE;
    typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);
    LPFN_ISWOW64PROCESS fnIsWow64Process = reinterpret_cast<LPFN_ISWOW64PROCESS>(
        GetProcAddress(GetModuleHandle(TEXT("kernel32")), "IsWow64Process"));

    if (fnIsWow64Process != nullptr) {
        fnIsWow64Process(GetCurrentProcess(), &isWow64);
    }
    return isWow64 == FALSE;  // If not WOW64 and not _WIN64, check native
#endif
#else
    // Non-Windows: check compile-time architecture
#if defined(__LP64__) || defined(_LP64) || defined(__x86_64__) || defined(__aarch64__)
    return true;
#else
    return false;
#endif
#endif
}

// =============================================================================
// Service Configuration Validation
// =============================================================================

core::Result<void, ServiceError> WindowsServiceWrapper::validateServiceConfig(
    const WindowsServiceConfig& config)
{
    if (config.serviceName.empty()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                        "Service name cannot be empty", 0}
        );
    }

    // Service name should not contain spaces (Windows requirement)
    if (config.serviceName.find(' ') != std::string::npos) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                        "Service name cannot contain spaces", 0}
        );
    }

    if (config.executablePath.empty()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                        "Executable path cannot be empty", 0}
        );
    }

    return core::Result<void, ServiceError>::success();
}

// =============================================================================
// Service Installation (Requirement 7.5)
// =============================================================================

core::Result<void, ServiceError> WindowsServiceWrapper::installService(
    const WindowsServiceConfig& config)
{
    auto validateResult = validateServiceConfig(config);
    if (validateResult.isError()) {
        return validateResult;
    }

#if defined(_WIN32)
    SC_HANDLE schSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (schSCManager == nullptr) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SCMConnectionFailed,
                        "Failed to open Service Control Manager",
                        GetLastError()}
        );
    }

    // Build command line
    std::string cmdLine = quotePathIfNeeded(config.executablePath);
    for (const auto& arg : config.arguments) {
        cmdLine += " " + quotePathIfNeeded(arg);
    }

    // Convert to wide string
    std::wstring wServiceName(config.serviceName.begin(), config.serviceName.end());
    std::wstring wDisplayName(config.displayName.begin(), config.displayName.end());
    std::wstring wCmdLine(cmdLine.begin(), cmdLine.end());

    DWORD startType = SERVICE_AUTO_START;
    switch (config.startType) {
        case ServiceStartType::AutoStart:
            startType = SERVICE_AUTO_START;
            break;
        case ServiceStartType::DemandStart:
            startType = SERVICE_DEMAND_START;
            break;
        case ServiceStartType::Disabled:
            startType = SERVICE_DISABLED;
            break;
        default:
            startType = SERVICE_AUTO_START;
            break;
    }

    SC_HANDLE schService = CreateServiceW(
        schSCManager,
        wServiceName.c_str(),
        wDisplayName.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        startType,
        SERVICE_ERROR_NORMAL,
        wCmdLine.c_str(),
        nullptr,  // No load ordering group
        nullptr,  // No tag identifier
        nullptr,  // No dependencies
        nullptr,  // LocalSystem account
        nullptr   // No password
    );

    if (schService == nullptr) {
        DWORD error = GetLastError();
        CloseServiceHandle(schSCManager);

        if (error == ERROR_SERVICE_EXISTS) {
            return core::Result<void, ServiceError>::error(
                ServiceError{ServiceErrorCode::ServiceAlreadyExists,
                            "Service already exists", error}
            );
        }

        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::Unknown,
                        "Failed to create service", error}
        );
    }

    // Set description if provided
    if (!config.description.empty()) {
        std::wstring wDescription(config.description.begin(), config.description.end());
        SERVICE_DESCRIPTIONW sd;
        sd.lpDescription = const_cast<LPWSTR>(wDescription.c_str());
        ChangeServiceConfig2W(schService, SERVICE_CONFIG_DESCRIPTION, &sd);
    }

    // Configure delayed auto-start if requested
    if (config.delayedAutoStart && config.startType == ServiceStartType::AutoStart) {
        SERVICE_DELAYED_AUTO_START_INFO delayedInfo;
        delayedInfo.fDelayedAutostart = TRUE;
        ChangeServiceConfig2W(schService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayedInfo);
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    return core::Result<void, ServiceError>::success();
#else
    // Non-Windows: return error
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::Unknown,
                    "Service installation not supported on this platform", 0}
    );
#endif
}

core::Result<void, ServiceError> WindowsServiceWrapper::uninstallService(
    const std::string& serviceName)
{
#if defined(_WIN32)
    SC_HANDLE schSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == nullptr) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SCMConnectionFailed,
                        "Failed to open Service Control Manager",
                        GetLastError()}
        );
    }

    std::wstring wServiceName(serviceName.begin(), serviceName.end());
    SC_HANDLE schService = OpenServiceW(schSCManager, wServiceName.c_str(), DELETE);

    if (schService == nullptr) {
        DWORD error = GetLastError();
        CloseServiceHandle(schSCManager);

        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return core::Result<void, ServiceError>::error(
                ServiceError{ServiceErrorCode::ServiceNotFound,
                            "Service not found", error}
            );
        }

        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::AccessDenied,
                        "Failed to open service for deletion", error}
        );
    }

    if (!DeleteService(schService)) {
        DWORD error = GetLastError();
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);

        if (error == ERROR_SERVICE_MARKED_FOR_DELETE) {
            return core::Result<void, ServiceError>::error(
                ServiceError{ServiceErrorCode::ServiceMarkedForDeletion,
                            "Service is already marked for deletion", error}
            );
        }

        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::Unknown,
                        "Failed to delete service", error}
        );
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    return core::Result<void, ServiceError>::success();
#else
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::Unknown,
                    "Service uninstallation not supported on this platform", 0}
    );
#endif
}

std::string WindowsServiceWrapper::generateInstallCommand(
    const WindowsServiceConfig& config) const
{
    std::ostringstream cmd;
    cmd << "sc create " << config.serviceName;
    cmd << " binPath= \"" << config.executablePath;

    for (const auto& arg : config.arguments) {
        cmd << " " << arg;
    }
    cmd << "\"";

    cmd << " DisplayName= \"" << config.displayName << "\"";

    switch (config.startType) {
        case ServiceStartType::AutoStart:
            cmd << " start= auto";
            break;
        case ServiceStartType::DemandStart:
            cmd << " start= demand";
            break;
        case ServiceStartType::Disabled:
            cmd << " start= disabled";
            break;
        default:
            cmd << " start= auto";
            break;
    }

    return cmd.str();
}

std::string WindowsServiceWrapper::generateUninstallCommand(
    const std::string& serviceName) const
{
    return "sc delete " + serviceName;
}

// =============================================================================
// Service Control
// =============================================================================

core::Result<void, ServiceError> WindowsServiceWrapper::startService(
    const std::string& serviceName)
{
#if defined(_WIN32)
    SC_HANDLE schSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (schSCManager == nullptr) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SCMConnectionFailed,
                        "Failed to open SCM", GetLastError()}
        );
    }

    std::wstring wServiceName(serviceName.begin(), serviceName.end());
    SC_HANDLE schService = OpenServiceW(schSCManager, wServiceName.c_str(), SERVICE_START);

    if (schService == nullptr) {
        DWORD error = GetLastError();
        CloseServiceHandle(schSCManager);
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::ServiceNotFound,
                        "Failed to open service", error}
        );
    }

    if (!StartService(schService, 0, nullptr)) {
        DWORD error = GetLastError();
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::Unknown,
                        "Failed to start service", error}
        );
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    return core::Result<void, ServiceError>::success();
#else
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::Unknown, "Not supported on this platform", 0}
    );
#endif
}

core::Result<void, ServiceError> WindowsServiceWrapper::stopService(
    const std::string& serviceName)
{
#if defined(_WIN32)
    SC_HANDLE schSCManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (schSCManager == nullptr) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SCMConnectionFailed,
                        "Failed to open SCM", GetLastError()}
        );
    }

    std::wstring wServiceName(serviceName.begin(), serviceName.end());
    SC_HANDLE schService = OpenServiceW(schSCManager, wServiceName.c_str(), SERVICE_STOP);

    if (schService == nullptr) {
        DWORD error = GetLastError();
        CloseServiceHandle(schSCManager);
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::ServiceNotFound,
                        "Failed to open service", error}
        );
    }

    SERVICE_STATUS status;
    if (!ControlService(schService, SERVICE_CONTROL_STOP, &status)) {
        DWORD error = GetLastError();
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::Unknown,
                        "Failed to stop service", error}
        );
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    return core::Result<void, ServiceError>::success();
#else
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::Unknown, "Not supported on this platform", 0}
    );
#endif
}

core::Result<ServiceStatusInfo, ServiceError> WindowsServiceWrapper::getServiceStatus() {
    ServiceStatusInfo info;
    info.state = currentState_.load();
    info.acceptedControls = ServiceAcceptedControl::Stop | ServiceAcceptedControl::Shutdown;

    return core::Result<ServiceStatusInfo, ServiceError>::success(info);
}

core::Result<void, ServiceError> WindowsServiceWrapper::setServiceStatus(
    const ServiceStatusInfo& status)
{
#if defined(_WIN32)
    if (serviceStatusHandle_ == nullptr) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::NotRunningAsService,
                        "Not running as a service", 0}
        );
    }

    SERVICE_STATUS serviceStatus;
    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = static_cast<DWORD>(status.state);
    serviceStatus.dwControlsAccepted = status.acceptedControls;
    serviceStatus.dwWin32ExitCode = status.win32ExitCode;
    serviceStatus.dwServiceSpecificExitCode = status.serviceSpecificExitCode;
    serviceStatus.dwCheckPoint = status.checkPoint;
    serviceStatus.dwWaitHint = status.waitHint;

    if (!SetServiceStatus(static_cast<SERVICE_STATUS_HANDLE>(serviceStatusHandle_),
                         &serviceStatus)) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::Unknown,
                        "Failed to set service status", GetLastError()}
        );
    }

    currentState_.store(status.state);

    return core::Result<void, ServiceError>::success();
#else
    currentState_.store(status.state);
    return core::Result<void, ServiceError>::success();
#endif
}

ServiceState WindowsServiceWrapper::getCurrentServiceState() const {
    return currentState_.load();
}

// =============================================================================
// SCM Event Handling (Requirement 7.5)
// =============================================================================

#if defined(_WIN32)
DWORD WINAPI WindowsServiceWrapper::serviceControlHandler(
    DWORD control, DWORD eventType, LPVOID eventData, LPVOID context)
{
    (void)eventType;
    (void)eventData;
    (void)context;

    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ == nullptr) {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    ServiceControlCode code = static_cast<ServiceControlCode>(control);

    switch (control) {
        case SERVICE_CONTROL_STOP:
            instance_->requestShutdown(ShutdownReason::ServiceStop);
            break;

        case SERVICE_CONTROL_SHUTDOWN:
            instance_->requestShutdown(ShutdownReason::SystemShutdown);
            break;

        case SERVICE_CONTROL_PRESHUTDOWN:
            instance_->requestShutdown(ShutdownReason::PreShutdown);
            break;

        case SERVICE_CONTROL_INTERROGATE:
            // Just return current state
            break;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }

    // Invoke user callback if set
    {
        std::lock_guard<std::mutex> callbackLock(instance_->stateMutex_);
        if (instance_->controlCallback_) {
            instance_->controlCallback_(code);
        }
    }

    return NO_ERROR;
}

void WINAPI WindowsServiceWrapper::serviceMain(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;

    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ == nullptr) {
        return;
    }

    // Register control handler
    instance_->serviceStatusHandle_ = RegisterServiceCtrlHandlerExW(
        L"",
        serviceControlHandler,
        nullptr
    );

    if (instance_->serviceStatusHandle_ == nullptr) {
        return;
    }

    // Report running status
    ServiceStatusInfo status;
    status.state = ServiceState::Running;
    status.acceptedControls = ServiceAcceptedControl::Stop |
                             ServiceAcceptedControl::Shutdown |
                             ServiceAcceptedControl::PreShutdown;
    instance_->setServiceStatus(status);

    // Call user's main callback
    if (instance_->mainCallback_) {
        instance_->mainCallback_();
    }
}
#endif

core::Result<void, ServiceError> WindowsServiceWrapper::registerControlHandler() {
#if defined(_WIN32)
    if (!isRunningAsService()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::NotRunningAsService,
                        "Not running as a Windows service", 0}
        );
    }

    return core::Result<void, ServiceError>::success();
#else
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::NotRunningAsService,
                    "Not running on Windows", 0}
    );
#endif
}

void WindowsServiceWrapper::setControlCallback(ServiceControlCallback callback) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    controlCallback_ = std::move(callback);
}

// =============================================================================
// Firewall Management (Requirement 7.6)
// =============================================================================

core::Result<void, ServiceError> WindowsServiceWrapper::validateFirewallRule(
    const FirewallRuleConfig& rule)
{
    if (rule.ruleName.empty()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                        "Firewall rule name cannot be empty", 0}
        );
    }

    if (rule.port == 0 && rule.applicationPath.empty()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                        "Firewall rule must specify port or application path", 0}
        );
    }

    return core::Result<void, ServiceError>::success();
}

core::Result<void, ServiceError> WindowsServiceWrapper::addFirewallRule(
    const FirewallRuleConfig& rule)
{
    auto validateResult = validateFirewallRule(rule);
    if (validateResult.isError()) {
        return validateResult;
    }

#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to initialize COM", static_cast<uint32_t>(hr)}
        );
    }

    INetFwPolicy2* pNetFwPolicy2 = nullptr;
    hr = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&pNetFwPolicy2)
    );

    if (FAILED(hr)) {
        CoUninitialize();
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to create firewall policy instance",
                        static_cast<uint32_t>(hr)}
        );
    }

    INetFwRules* pFwRules = nullptr;
    hr = pNetFwPolicy2->get_Rules(&pFwRules);
    if (FAILED(hr)) {
        pNetFwPolicy2->Release();
        CoUninitialize();
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to get firewall rules", static_cast<uint32_t>(hr)}
        );
    }

    INetFwRule* pFwRule = nullptr;
    hr = CoCreateInstance(
        __uuidof(NetFwRule),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwRule),
        reinterpret_cast<void**>(&pFwRule)
    );

    if (FAILED(hr)) {
        pFwRules->Release();
        pNetFwPolicy2->Release();
        CoUninitialize();
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to create firewall rule", static_cast<uint32_t>(hr)}
        );
    }

    // Configure rule
    std::wstring wRuleName(rule.ruleName.begin(), rule.ruleName.end());
    std::wstring wDescription(rule.description.begin(), rule.description.end());

    pFwRule->put_Name(_bstr_t(wRuleName.c_str()));
    pFwRule->put_Description(_bstr_t(wDescription.c_str()));

    if (!rule.applicationPath.empty()) {
        std::wstring wAppPath(rule.applicationPath.begin(), rule.applicationPath.end());
        pFwRule->put_ApplicationName(_bstr_t(wAppPath.c_str()));
    }

    // Set protocol
    NET_FW_IP_PROTOCOL protocol = NET_FW_IP_PROTOCOL_TCP;
    if (rule.protocol == FirewallProtocol::UDP) {
        protocol = NET_FW_IP_PROTOCOL_UDP;
    }
    pFwRule->put_Protocol(protocol);

    // Set port
    if (rule.port > 0) {
        std::wstring portStr = std::to_wstring(rule.port);
        pFwRule->put_LocalPorts(_bstr_t(portStr.c_str()));
    }

    // Set direction
    NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
    if (rule.direction == FirewallDirection::Outbound) {
        direction = NET_FW_RULE_DIR_OUT;
    }
    pFwRule->put_Direction(direction);

    // Set action
    NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
    if (rule.action == FirewallAction::Block) {
        action = NET_FW_ACTION_BLOCK;
    }
    pFwRule->put_Action(action);

    // Set profiles (all profiles by default)
    pFwRule->put_Profiles(NET_FW_PROFILE2_ALL);

    // Enable rule
    pFwRule->put_Enabled(rule.enabled ? VARIANT_TRUE : VARIANT_FALSE);

    // Add the rule
    hr = pFwRules->Add(pFwRule);

    pFwRule->Release();
    pFwRules->Release();
    pNetFwPolicy2->Release();
    CoUninitialize();

    if (FAILED(hr)) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to add firewall rule", static_cast<uint32_t>(hr)}
        );
    }

    return core::Result<void, ServiceError>::success();
#else
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::FirewallError,
                    "Firewall management not supported on this platform", 0}
    );
#endif
}

core::Result<void, ServiceError> WindowsServiceWrapper::removeFirewallRule(
    const std::string& ruleName)
{
#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to initialize COM", static_cast<uint32_t>(hr)}
        );
    }

    INetFwPolicy2* pNetFwPolicy2 = nullptr;
    hr = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&pNetFwPolicy2)
    );

    if (FAILED(hr)) {
        CoUninitialize();
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to create firewall policy", static_cast<uint32_t>(hr)}
        );
    }

    INetFwRules* pFwRules = nullptr;
    hr = pNetFwPolicy2->get_Rules(&pFwRules);
    if (FAILED(hr)) {
        pNetFwPolicy2->Release();
        CoUninitialize();
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to get firewall rules", static_cast<uint32_t>(hr)}
        );
    }

    std::wstring wRuleName(ruleName.begin(), ruleName.end());
    hr = pFwRules->Remove(_bstr_t(wRuleName.c_str()));

    pFwRules->Release();
    pNetFwPolicy2->Release();
    CoUninitialize();

    if (FAILED(hr)) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallRuleNotFound,
                        "Failed to remove firewall rule", static_cast<uint32_t>(hr)}
        );
    }

    return core::Result<void, ServiceError>::success();
#else
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::FirewallError,
                    "Not supported on this platform", 0}
    );
#endif
}

core::Result<bool, ServiceError> WindowsServiceWrapper::firewallRuleExists(
    const std::string& ruleName)
{
#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return core::Result<bool, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to initialize COM", static_cast<uint32_t>(hr)}
        );
    }

    INetFwPolicy2* pNetFwPolicy2 = nullptr;
    hr = CoCreateInstance(
        __uuidof(NetFwPolicy2),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2),
        reinterpret_cast<void**>(&pNetFwPolicy2)
    );

    if (FAILED(hr)) {
        CoUninitialize();
        return core::Result<bool, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to create firewall policy", static_cast<uint32_t>(hr)}
        );
    }

    INetFwRules* pFwRules = nullptr;
    hr = pNetFwPolicy2->get_Rules(&pFwRules);
    if (FAILED(hr)) {
        pNetFwPolicy2->Release();
        CoUninitialize();
        return core::Result<bool, ServiceError>::error(
            ServiceError{ServiceErrorCode::FirewallError,
                        "Failed to get firewall rules", static_cast<uint32_t>(hr)}
        );
    }

    std::wstring wRuleName(ruleName.begin(), ruleName.end());
    INetFwRule* pFwRule = nullptr;
    hr = pFwRules->Item(_bstr_t(wRuleName.c_str()), &pFwRule);

    bool exists = SUCCEEDED(hr) && pFwRule != nullptr;
    if (pFwRule) {
        pFwRule->Release();
    }

    pFwRules->Release();
    pNetFwPolicy2->Release();
    CoUninitialize();

    return core::Result<bool, ServiceError>::success(exists);
#else
    return core::Result<bool, ServiceError>::success(false);
#endif
}

// =============================================================================
// Service Execution Modes
// =============================================================================

bool WindowsServiceWrapper::isRunningAsService() const {
#if defined(_WIN32)
    // Check if we're running with console
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE || hConsole == nullptr) {
        return true;  // No console - likely running as service
    }

    // Check if parent process is services.exe
    // This is a simplified check; a more thorough check would trace process tree

    return false;
#else
    return false;
#endif
}

bool WindowsServiceWrapper::isConsoleApplication() const {
    return !isRunningAsService();
}

core::Result<void, ServiceError> WindowsServiceWrapper::runAsService(
    const std::string& serviceName,
    ServiceMainCallback mainCallback,
    ServiceStopCallback stopCallback)
{
#if defined(_WIN32)
    mainCallback_ = mainCallback;
    stopCallback_ = stopCallback;

    std::wstring wServiceName(serviceName.begin(), serviceName.end());

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(wServiceName.c_str()), serviceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SCMConnectionFailed,
                        "Failed to start service control dispatcher",
                        GetLastError()}
        );
    }

    return core::Result<void, ServiceError>::success();
#else
    return core::Result<void, ServiceError>::error(
        ServiceError{ServiceErrorCode::NotRunningAsService,
                    "Not running on Windows", 0}
    );
#endif
}

core::Result<void, ServiceError> WindowsServiceWrapper::runAsConsole(
    ServiceMainCallback mainCallback,
    ServiceStopCallback stopCallback)
{
    mainCallback_ = mainCallback;
    stopCallback_ = stopCallback;
    currentState_.store(ServiceState::Running);

#if defined(_WIN32)
    // Set up console control handler
    SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL {
        std::lock_guard<std::mutex> lock(instanceMutex_);
        if (instance_ != nullptr) {
            switch (ctrlType) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                case CTRL_SHUTDOWN_EVENT:
                    instance_->requestShutdown(ShutdownReason::UserRequested);
                    return TRUE;
            }
        }
        return FALSE;
    }, TRUE);
#endif

    // Call main callback
    if (mainCallback) {
        bool result = mainCallback();
        if (!result) {
            return core::Result<void, ServiceError>::error(
                ServiceError{ServiceErrorCode::Unknown,
                            "Main callback returned failure", 0}
            );
        }
    }

    return core::Result<void, ServiceError>::success();
}

// =============================================================================
// Shutdown Management
// =============================================================================

bool WindowsServiceWrapper::isShutdownRequested() const {
    return shutdownRequested_.load();
}

void WindowsServiceWrapper::requestShutdown(ShutdownReason reason) {
    shutdownReason_.store(reason);
    shutdownRequested_.store(true);
    currentState_.store(ServiceState::StopPending);

    // Invoke shutdown callback
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (shutdownCallback_) {
        shutdownCallback_(reason);
    }

    // Call stop callback
    if (stopCallback_) {
        stopCallback_();
    }
}

ShutdownReason WindowsServiceWrapper::getShutdownReason() const {
    return shutdownReason_.load();
}

bool WindowsServiceWrapper::waitForShutdown(std::chrono::milliseconds timeout) {
    auto endTime = std::chrono::steady_clock::now() + timeout;

    while (!shutdownRequested_.load()) {
        if (std::chrono::steady_clock::now() >= endTime) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return true;
}

void WindowsServiceWrapper::setGracePeriod(std::chrono::seconds period) {
    gracePeriod_ = period;
}

std::chrono::seconds WindowsServiceWrapper::getGracePeriod() const {
    return gracePeriod_;
}

void WindowsServiceWrapper::setShutdownCallback(ShutdownCallback callback) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    shutdownCallback_ = std::move(callback);
}

// =============================================================================
// Event Logging
// =============================================================================

void WindowsServiceWrapper::logEvent(EventLogType type, uint32_t eventId,
                                     const std::string& message)
{
#if defined(_WIN32)
    if (eventLogHandle_ == nullptr) {
        eventLogHandle_ = RegisterEventSourceW(nullptr, L"OpenRTMP");
        if (eventLogHandle_ == nullptr) {
            return;  // Can't log - just return
        }
    }

    WORD eventType = EVENTLOG_INFORMATION_TYPE;
    switch (type) {
        case EventLogType::Information:
            eventType = EVENTLOG_INFORMATION_TYPE;
            break;
        case EventLogType::Warning:
            eventType = EVENTLOG_WARNING_TYPE;
            break;
        case EventLogType::Error:
            eventType = EVENTLOG_ERROR_TYPE;
            break;
    }

    std::wstring wMessage(message.begin(), message.end());
    LPCWSTR strings[1] = { wMessage.c_str() };

    ReportEventW(
        static_cast<HANDLE>(eventLogHandle_),
        eventType,
        0,          // Category
        eventId,
        nullptr,    // User SID
        1,          // Number of strings
        0,          // Data size
        strings,
        nullptr     // Data
    );
#else
    // Non-Windows: just ignore
    (void)type;
    (void)eventId;
    (void)message;
#endif
}

// =============================================================================
// Helper Methods
// =============================================================================

std::string WindowsServiceWrapper::quotePathIfNeeded(const std::string& path) const {
    if (path.empty()) {
        return path;
    }

    if (path.front() == '"' && path.back() == '"') {
        return path;  // Already quoted
    }

    if (path.find(' ') != std::string::npos) {
        return "\"" + path + "\"";
    }

    return path;
}

} // namespace windows
} // namespace pal
} // namespace openrtmp
