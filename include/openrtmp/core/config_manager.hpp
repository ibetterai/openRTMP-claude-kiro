// OpenRTMP - Cross-platform RTMP Server
// Configuration Manager - Handles configuration loading and validation
//
// Responsibilities:
// - Parse JSON and YAML configuration file formats
// - Support environment variable overrides for containerized deployments
// - Validate configuration schema on startup with detailed error messages
// - Apply sensible defaults when configuration file is absent
// - Log effective configuration values during initialization
//
// Requirements coverage:
// - Requirement 17.1: Configuration through JSON or YAML file
// - Requirement 17.2: Configuration through environment variables
// - Requirement 17.4: Validate configuration on startup
// - Requirement 17.5: Sensible defaults when config absent

#ifndef OPENRTMP_CORE_CONFIG_MANAGER_HPP
#define OPENRTMP_CORE_CONFIG_MANAGER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "openrtmp/core/result.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// Log Level Enumeration
// =============================================================================

/**
 * @brief Logging level enumeration.
 */
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

// =============================================================================
// Configuration Parameter Enumeration
// =============================================================================

/**
 * @brief Enumeration of all configurable parameters.
 *
 * Used to identify specific configuration parameters for hot-reload
 * and change notification purposes.
 */
enum class ConfigParameter {
    // Server parameters (require restart)
    ServerPort,
    RtmpsPort,
    BindAddress,
    MaxConnections,
    MaxPublishers,

    // Logging parameters (hot-reloadable)
    LogLevel,
    EnableConsoleLog,
    EnableFileLog,
    LogFilePath,

    // Authentication parameters (hot-reloadable)
    PublishAuthEnabled,
    SubscribeAuthEnabled,
    AuthTimeout,
    RateLimitPerMinute,

    // TLS parameters (require restart)
    TlsEnabled,
    TlsCertPath,
    TlsKeyPath,

    // Streaming parameters (hot-reloadable)
    GopBufferSeconds,
    MaxSubscriberBuffer,
    LowLatencyMode,
    LowLatencyBuffer,
    MaxBitrate,

    // Timeout parameters (hot-reloadable)
    HandshakeTimeout,
    ConnectionTimeout
};

// =============================================================================
// Configuration Change Event
// =============================================================================

/**
 * @brief Event emitted when a configuration parameter changes.
 */
struct ConfigChangeEvent {
    ConfigParameter parameter;  ///< Which parameter changed
    std::string oldValue;       ///< Previous value as string
    std::string newValue;       ///< New value as string
    std::chrono::steady_clock::time_point timestamp;  ///< When the change occurred
};

/**
 * @brief Callback type for configuration change notifications.
 */
using ConfigChangeCallback = std::function<void(const ConfigChangeEvent&)>;

// =============================================================================
// Parameter Info
// =============================================================================

/**
 * @brief Information about a configuration parameter.
 */
struct ParameterInfo {
    std::string name;           ///< Human-readable parameter name
    std::string description;    ///< Description of the parameter
    bool requiresRestart;       ///< True if change requires server restart
};

// =============================================================================
// Runtime Configuration Update
// =============================================================================

/**
 * @brief Structure for hot-reloadable runtime configuration updates.
 *
 * Only includes parameters that can be changed without server restart.
 * Use std::optional to indicate which fields should be updated.
 */
struct RuntimeConfigUpdate {
    // Logging
    std::optional<LogLevel> logLevel;
    std::optional<bool> enableConsoleLog;
    std::optional<bool> enableFileLog;

    // Streaming
    std::optional<double> gopBufferSeconds;
    std::optional<uint32_t> maxSubscriberBufferMs;
    std::optional<bool> lowLatencyMode;
    std::optional<uint32_t> lowLatencyBufferMs;

    // Timeouts
    std::optional<uint32_t> handshakeTimeoutMs;
    std::optional<uint32_t> connectionTimeoutMs;

    // Authentication
    std::optional<bool> publishAuthEnabled;
    std::optional<bool> subscribeAuthEnabled;
    std::optional<uint32_t> authTimeoutMs;
    std::optional<uint32_t> rateLimitPerMinute;
};

// =============================================================================
// Hot Reload Result
// =============================================================================

/**
 * @brief Result of a hot-reload operation.
 */
struct HotReloadResult {
    std::vector<ConfigParameter> appliedChanges;   ///< Successfully applied changes
    std::vector<ConfigParameter> skippedChanges;   ///< Changes that require restart
};

// =============================================================================
// Configuration Format Enumeration
// =============================================================================

/**
 * @brief Configuration file format.
 */
enum class ConfigFormat {
    JSON,
    YAML
};

// =============================================================================
// Configuration Structures
// =============================================================================

/**
 * @brief Server configuration section.
 */
struct ServerConfig {
    uint16_t port = 1935;                 ///< RTMP port (default: 1935)
    uint16_t rtmpsPort = 1936;            ///< RTMPS port (default: 1936)
    std::string bindAddress = "0.0.0.0";  ///< Bind address (default: all interfaces)
    uint32_t maxConnections = 1000;       ///< Maximum concurrent connections
    uint32_t maxPublishers = 10;          ///< Maximum simultaneous publishers
    uint32_t handshakeTimeoutMs = 10000;  ///< Handshake timeout in milliseconds
    uint32_t connectionTimeoutMs = 30000; ///< Connection timeout in milliseconds
};

/**
 * @brief Logging configuration section.
 */
struct LoggingConfig {
    LogLevel level = LogLevel::Info;      ///< Log level (default: info)
    bool enableConsole = false;           ///< Enable console output
    bool enableFile = false;              ///< Enable file output
    std::string filePath;                 ///< Log file path
    bool enableJson = false;              ///< Enable JSON structured logging
    uint32_t maxFileSizeMB = 100;         ///< Max log file size in MB
    uint32_t maxFiles = 5;                ///< Max number of rotated files
};

/**
 * @brief Authentication configuration section.
 */
struct AuthenticationConfig {
    bool publishAuthEnabled = true;       ///< Enable auth for publish
    bool subscribeAuthEnabled = false;    ///< Enable auth for subscribe
    std::vector<std::string> streamKeys;  ///< Configured stream keys
    std::string externalAuthUrl;          ///< External auth callback URL
    uint32_t authTimeoutMs = 5000;        ///< Auth callback timeout
    uint32_t rateLimitPerMinute = 5;      ///< Failed auth attempts per IP per minute
};

/**
 * @brief TLS configuration section for config manager.
 *
 * Note: Named ConfigTLS to avoid conflict with TLSConfig in tls_service.hpp
 */
struct ConfigTLS {
    bool enabled = false;                 ///< Enable RTMPS
    std::string certPath;                 ///< Certificate file path
    std::string keyPath;                  ///< Private key file path
    std::string caPath;                   ///< CA certificate path (for client verification)
    std::string minVersion = "TLS1.2";    ///< Minimum TLS version
    bool verifyClient = false;            ///< Require client certificate
    std::string pinnedCertHash;           ///< Pinned certificate hash
};

/**
 * @brief Streaming configuration section.
 */
struct StreamingConfig {
    double gopBufferSeconds = 2.0;        ///< GOP buffer duration in seconds
    uint32_t maxSubscriberBufferMs = 5000;///< Max subscriber buffer in milliseconds
    bool lowLatencyMode = false;          ///< Enable low latency mode
    uint32_t lowLatencyBufferMs = 500;    ///< Low latency buffer limit
    uint32_t maxBitrateMbps = 50;         ///< Max stream bitrate in Mbps
    uint32_t maxPublishers = 10;          ///< Max simultaneous publishers
    uint32_t maxSubscribersPerStream = 100;///< Max subscribers per stream
};

/**
 * @brief Network configuration section for mobile platforms.
 */
struct NetworkConfig {
    bool allowCellular = false;           ///< Allow cellular network
    bool allowWiFiOnly = true;            ///< Restrict to WiFi only
    uint32_t networkChangeGraceMs = 30000;///< Grace period for network changes
    bool warnOnCellular = true;           ///< Warn user when using cellular
};

/**
 * @brief Complete server configuration.
 */
struct Configuration {
    ServerConfig server;
    LoggingConfig logging;
    AuthenticationConfig auth;
    ConfigTLS tls;
    StreamingConfig streaming;
    NetworkConfig network;
};

// =============================================================================
// Configuration Error
// =============================================================================

/**
 * @brief Configuration error with detailed information.
 */
struct ConfigError {
    enum class Code {
        None,
        FileNotFound,
        ParseError,
        ValidationError,
        UnsupportedFormat,
        IOError
    };

    Code code = Code::None;
    std::string message;
    std::string field;        ///< Field that caused the error (if applicable)
    int line = -1;            ///< Line number in config file (if applicable)

    ConfigError() = default;
    ConfigError(Code c, std::string msg) : code(c), message(std::move(msg)) {}
    ConfigError(Code c, std::string msg, std::string f)
        : code(c), message(std::move(msg)), field(std::move(f)) {}
    ConfigError(Code c, std::string msg, std::string f, int l)
        : code(c), message(std::move(msg)), field(std::move(f)), line(l) {}
};

// =============================================================================
// Configuration Manager Interface
// =============================================================================

/**
 * @brief Log callback type for configuration logging.
 */
using ConfigLogCallback = std::function<void(const std::string&)>;

/**
 * @brief Configuration manager for loading and validating server configuration.
 *
 * Supports:
 * - JSON and YAML configuration file formats
 * - Environment variable overrides (OPENRTMP_* prefix)
 * - Schema validation with detailed error messages
 * - Sensible defaults when configuration is absent
 * - Logging of effective configuration values
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses shared_mutex for read/write locking
 *
 * Usage example:
 * @code
 * ConfigManager manager;
 * manager.setLogCallback([](const std::string& msg) {
 *     std::cout << msg << std::endl;
 * });
 *
 * // Load from file (if exists) or use defaults
 * auto result = manager.loadFromFile("config.json");
 * if (result.isError()) {
 *     // Use defaults
 *     manager.loadDefaults();
 * }
 *
 * // Apply environment variable overrides
 * manager.applyEnvironmentOverrides();
 *
 * // Validate final configuration
 * auto validateResult = manager.validate();
 * if (validateResult.isError()) {
 *     std::cerr << "Config error: " << validateResult.error().message;
 *     return 1;
 * }
 *
 * // Use configuration
 * const auto& config = manager.getConfig();
 * @endcode
 */
class ConfigManager {
public:
    /**
     * @brief Construct a ConfigManager with default configuration.
     */
    ConfigManager();

    /**
     * @brief Destructor.
     */
    ~ConfigManager();

    // Non-copyable and non-movable (contains mutex)
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    // -------------------------------------------------------------------------
    // Configuration Loading
    // -------------------------------------------------------------------------

    /**
     * @brief Load configuration from a file.
     *
     * Automatically detects format based on file extension:
     * - .json: JSON format
     * - .yaml, .yml: YAML format
     *
     * @param filePath Path to configuration file
     * @return Result with void on success, ConfigError on failure
     */
    Result<void, ConfigError> loadFromFile(const std::string& filePath);

    /**
     * @brief Load configuration from a JSON string.
     *
     * @param jsonContent JSON configuration content
     * @return Result with void on success, ConfigError on failure
     */
    Result<void, ConfigError> loadFromJsonString(const std::string& jsonContent);

    /**
     * @brief Load configuration from a YAML string.
     *
     * @param yamlContent YAML configuration content
     * @return Result with void on success, ConfigError on failure
     */
    Result<void, ConfigError> loadFromYamlString(const std::string& yamlContent);

    /**
     * @brief Load sensible default configuration.
     *
     * Uses platform-appropriate defaults:
     * - Desktop: Higher limits (1000 connections, 10 publishers)
     * - Mobile: Lower limits (100 connections, 3 publishers)
     *
     * @return Result with void on success
     */
    Result<void, ConfigError> loadDefaults();

    // -------------------------------------------------------------------------
    // Environment Variable Overrides
    // -------------------------------------------------------------------------

    /**
     * @brief Apply environment variable overrides.
     *
     * Supported environment variables:
     * - OPENRTMP_PORT: Server port
     * - OPENRTMP_RTMPS_PORT: RTMPS port
     * - OPENRTMP_BIND_ADDRESS: Bind address
     * - OPENRTMP_MAX_CONNECTIONS: Max connections
     * - OPENRTMP_LOG_LEVEL: Log level (debug|info|warning|error)
     * - OPENRTMP_TLS_ENABLED: Enable TLS (true|false)
     * - OPENRTMP_TLS_CERT_PATH: TLS certificate path
     * - OPENRTMP_TLS_KEY_PATH: TLS private key path
     * - OPENRTMP_AUTH_PUBLISH_ENABLED: Enable publish auth (true|false)
     * - OPENRTMP_AUTH_SUBSCRIBE_ENABLED: Enable subscribe auth (true|false)
     *
     * Environment variables override file configuration values.
     */
    void applyEnvironmentOverrides();

    // -------------------------------------------------------------------------
    // Validation
    // -------------------------------------------------------------------------

    /**
     * @brief Validate the current configuration.
     *
     * Validates:
     * - Port ranges (1-65535)
     * - Required fields (TLS cert/key when TLS enabled)
     * - Value ranges (maxConnections > 0, gopBuffer >= 0)
     * - Enumeration values (log level)
     *
     * @return Result with void on success, ConfigError on failure
     */
    Result<void, ConfigError> validate() const;

    // -------------------------------------------------------------------------
    // Configuration Access
    // -------------------------------------------------------------------------

    /**
     * @brief Get the current configuration.
     *
     * @return Const reference to current configuration
     */
    const Configuration& getConfig() const;

    /**
     * @brief Dump the effective configuration as a string.
     *
     * @param format Output format (JSON or YAML)
     * @return Configuration as formatted string
     */
    std::string dumpConfig(ConfigFormat format = ConfigFormat::JSON) const;

    // -------------------------------------------------------------------------
    // Platform Settings
    // -------------------------------------------------------------------------

    /**
     * @brief Set whether this is a mobile platform.
     *
     * Affects default values for connection limits.
     *
     * @param isMobile True if running on mobile platform
     */
    void setMobilePlatform(bool isMobile);

    /**
     * @brief Check if configured for mobile platform.
     *
     * @return True if mobile platform settings apply
     */
    bool isMobilePlatform() const;

    // -------------------------------------------------------------------------
    // Logging
    // -------------------------------------------------------------------------

    /**
     * @brief Set the configuration log callback.
     *
     * Callback is invoked during configuration loading to log
     * effective configuration values.
     *
     * @param callback Callback function for log messages
     */
    void setLogCallback(ConfigLogCallback callback);

    // -------------------------------------------------------------------------
    // Runtime Configuration (Hot-Reload) - Requirement 17.3, 17.6
    // -------------------------------------------------------------------------

    /**
     * @brief Apply a runtime configuration update.
     *
     * Only updates hot-reloadable parameters. Validates changes before
     * applying and emits change events for each modified parameter.
     *
     * @param update The runtime configuration update to apply
     * @return Result with void on success, ConfigError on validation failure
     */
    Result<void, ConfigError> applyRuntimeUpdate(const RuntimeConfigUpdate& update);

    /**
     * @brief Hot-reload configuration from a file.
     *
     * Loads the new configuration file but only applies changes to
     * hot-reloadable parameters. Parameters requiring restart are
     * skipped and reported in the result.
     *
     * @param filePath Path to configuration file
     * @return Result with HotReloadResult on success, ConfigError on failure
     */
    Result<HotReloadResult, ConfigError> hotReloadFromFile(const std::string& filePath);

    /**
     * @brief Check if a parameter requires server restart to change.
     *
     * @param param The configuration parameter to check
     * @return True if the parameter requires restart, false if hot-reloadable
     */
    bool requiresRestart(ConfigParameter param) const;

    /**
     * @brief Get list of hot-reloadable parameters.
     *
     * @return Vector of parameters that can be changed without restart
     */
    std::vector<ConfigParameter> getHotReloadableParameters() const;

    /**
     * @brief Get information about all configuration parameters.
     *
     * Returns a map of parameter enums to their info including name,
     * description, and whether they require restart.
     *
     * @return Map of ConfigParameter to ParameterInfo
     */
    std::map<ConfigParameter, ParameterInfo> getParameterInfo() const;

    /**
     * @brief Set callback for configuration change notifications.
     *
     * The callback is invoked for each parameter that changes during
     * runtime updates. Events include old and new values.
     *
     * @param callback Callback function for change events
     */
    void setConfigChangeCallback(ConfigChangeCallback callback);

private:
    /**
     * @brief Parse JSON content into configuration.
     */
    Result<void, ConfigError> parseJson(const std::string& content);

    /**
     * @brief Parse YAML content into configuration.
     */
    Result<void, ConfigError> parseYaml(const std::string& content);

    /**
     * @brief Detect file format from extension.
     */
    ConfigFormat detectFormat(const std::string& filePath) const;

    /**
     * @brief Read file content.
     */
    Result<std::string, ConfigError> readFile(const std::string& filePath) const;

    /**
     * @brief Parse log level string.
     */
    std::optional<LogLevel> parseLogLevel(const std::string& level) const;

    /**
     * @brief Log a configuration message.
     */
    void log(const std::string& message) const;

    /**
     * @brief Log effective configuration.
     */
    void logEffectiveConfig() const;

    /**
     * @brief Get environment variable value.
     */
    std::optional<std::string> getEnvVar(const std::string& name) const;

    /**
     * @brief Validate a runtime update before applying.
     */
    Result<void, ConfigError> validateRuntimeUpdate(const RuntimeConfigUpdate& update) const;

    /**
     * @brief Emit a configuration change event.
     */
    void emitChangeEvent(ConfigParameter param,
                         const std::string& oldValue,
                         const std::string& newValue);

    /**
     * @brief Parse a configuration into a temporary Configuration object.
     */
    Result<Configuration, ConfigError> parseConfigFromFile(const std::string& filePath) const;

    // Configuration state
    Configuration config_;
    mutable std::shared_mutex configMutex_;

    // Platform setting
    bool isMobile_ = false;

    // Logging callback
    ConfigLogCallback logCallback_;
    mutable std::mutex logMutex_;

    // Change notification callback
    ConfigChangeCallback changeCallback_;
    mutable std::mutex changeCallbackMutex_;
};

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Convert LogLevel to string.
 */
inline std::string logLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
        default: return "unknown";
    }
}

/**
 * @brief Parse string to LogLevel for config manager.
 *
 * Note: Named differently from structured_logger's stringToLogLevel to avoid
 * name collision in the same namespace.
 */
inline std::optional<LogLevel> parseLogLevelString(const std::string& str) {
    if (str == "debug") return LogLevel::Debug;
    if (str == "info") return LogLevel::Info;
    if (str == "warning") return LogLevel::Warning;
    if (str == "error") return LogLevel::Error;
    return std::nullopt;
}

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_CONFIG_MANAGER_HPP
