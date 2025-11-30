// OpenRTMP - Cross-platform RTMP Server
// Configuration Manager Implementation

#include "openrtmp/core/config_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace openrtmp {
namespace core {

// =============================================================================
// Simple JSON Parser (Minimal implementation for configuration)
// =============================================================================

namespace {

enum class JsonType {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    bool isNull() const { return type == JsonType::Null; }
    bool isBool() const { return type == JsonType::Boolean; }
    bool isNumber() const { return type == JsonType::Number; }
    bool isString() const { return type == JsonType::String; }
    bool isArray() const { return type == JsonType::Array; }
    bool isObject() const { return type == JsonType::Object; }

    bool getBool(bool defaultVal = false) const {
        return isBool() ? boolValue : defaultVal;
    }

    int64_t getInt(int64_t defaultVal = 0) const {
        return isNumber() ? static_cast<int64_t>(numberValue) : defaultVal;
    }

    double getDouble(double defaultVal = 0.0) const {
        return isNumber() ? numberValue : defaultVal;
    }

    const std::string& getString(const std::string& defaultVal = "") const {
        static const std::string empty;
        return isString() ? stringValue : (defaultVal.empty() ? empty : defaultVal);
    }

    bool contains(const std::string& key) const {
        return isObject() && objectValue.find(key) != objectValue.end();
    }

    const JsonValue& operator[](const std::string& key) const {
        static const JsonValue nullValue;
        if (!isObject()) return nullValue;
        auto it = objectValue.find(key);
        return it != objectValue.end() ? it->second : nullValue;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input), pos_(0) {}

    Result<JsonValue, ConfigError> parse() {
        skipWhitespace();
        auto result = parseValue();
        skipWhitespace();
        if (pos_ < input_.size()) {
            return Result<JsonValue, ConfigError>::error(
                ConfigError(ConfigError::Code::ParseError,
                           "Unexpected characters after JSON value"));
        }
        return result;
    }

private:
    const std::string& input_;
    size_t pos_;

    void skipWhitespace() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            pos_++;
        }
    }

    char peek() const {
        return pos_ < input_.size() ? input_[pos_] : '\0';
    }

    char consume() {
        return pos_ < input_.size() ? input_[pos_++] : '\0';
    }

    bool match(char c) {
        if (peek() == c) {
            consume();
            return true;
        }
        return false;
    }

    Result<JsonValue, ConfigError> parseValue() {
        skipWhitespace();
        char c = peek();

        if (c == '"') return parseString();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();

        return Result<JsonValue, ConfigError>::error(
            ConfigError(ConfigError::Code::ParseError,
                       "Unexpected character: " + std::string(1, c)));
    }

    Result<JsonValue, ConfigError> parseString() {
        if (!match('"')) {
            return Result<JsonValue, ConfigError>::error(
                ConfigError(ConfigError::Code::ParseError, "Expected '\"'"));
        }

        std::string result;
        while (pos_ < input_.size() && peek() != '"') {
            char c = consume();
            if (c == '\\') {
                char escaped = consume();
                switch (escaped) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    default: result += escaped; break;
                }
            } else {
                result += c;
            }
        }

        if (!match('"')) {
            return Result<JsonValue, ConfigError>::error(
                ConfigError(ConfigError::Code::ParseError, "Unterminated string"));
        }

        JsonValue value;
        value.type = JsonType::String;
        value.stringValue = std::move(result);
        return Result<JsonValue, ConfigError>::success(std::move(value));
    }

    Result<JsonValue, ConfigError> parseNumber() {
        size_t start = pos_;
        if (peek() == '-') consume();

        while (std::isdigit(static_cast<unsigned char>(peek()))) consume();

        if (peek() == '.') {
            consume();
            while (std::isdigit(static_cast<unsigned char>(peek()))) consume();
        }

        if (peek() == 'e' || peek() == 'E') {
            consume();
            if (peek() == '+' || peek() == '-') consume();
            while (std::isdigit(static_cast<unsigned char>(peek()))) consume();
        }

        std::string numStr = input_.substr(start, pos_ - start);
        try {
            JsonValue value;
            value.type = JsonType::Number;
            value.numberValue = std::stod(numStr);
            return Result<JsonValue, ConfigError>::success(std::move(value));
        } catch (...) {
            return Result<JsonValue, ConfigError>::error(
                ConfigError(ConfigError::Code::ParseError,
                           "Invalid number: " + numStr));
        }
    }

    Result<JsonValue, ConfigError> parseBool() {
        if (input_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            JsonValue value;
            value.type = JsonType::Boolean;
            value.boolValue = true;
            return Result<JsonValue, ConfigError>::success(std::move(value));
        }
        if (input_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            JsonValue value;
            value.type = JsonType::Boolean;
            value.boolValue = false;
            return Result<JsonValue, ConfigError>::success(std::move(value));
        }
        return Result<JsonValue, ConfigError>::error(
            ConfigError(ConfigError::Code::ParseError, "Expected 'true' or 'false'"));
    }

    Result<JsonValue, ConfigError> parseNull() {
        if (input_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            JsonValue value;
            value.type = JsonType::Null;
            return Result<JsonValue, ConfigError>::success(std::move(value));
        }
        return Result<JsonValue, ConfigError>::error(
            ConfigError(ConfigError::Code::ParseError, "Expected 'null'"));
    }

    Result<JsonValue, ConfigError> parseArray() {
        if (!match('[')) {
            return Result<JsonValue, ConfigError>::error(
                ConfigError(ConfigError::Code::ParseError, "Expected '['"));
        }

        JsonValue value;
        value.type = JsonType::Array;

        skipWhitespace();
        if (match(']')) {
            return Result<JsonValue, ConfigError>::success(std::move(value));
        }

        while (true) {
            auto elementResult = parseValue();
            if (elementResult.isError()) {
                return elementResult;
            }
            value.arrayValue.push_back(std::move(elementResult.value()));

            skipWhitespace();
            if (match(']')) break;
            if (!match(',')) {
                return Result<JsonValue, ConfigError>::error(
                    ConfigError(ConfigError::Code::ParseError,
                               "Expected ',' or ']' in array"));
            }
        }

        return Result<JsonValue, ConfigError>::success(std::move(value));
    }

    Result<JsonValue, ConfigError> parseObject() {
        if (!match('{')) {
            return Result<JsonValue, ConfigError>::error(
                ConfigError(ConfigError::Code::ParseError, "Expected '{'"));
        }

        JsonValue value;
        value.type = JsonType::Object;

        skipWhitespace();
        if (match('}')) {
            return Result<JsonValue, ConfigError>::success(std::move(value));
        }

        while (true) {
            skipWhitespace();
            auto keyResult = parseString();
            if (keyResult.isError()) {
                return Result<JsonValue, ConfigError>::error(
                    ConfigError(ConfigError::Code::ParseError,
                               "Expected string key in object"));
            }
            std::string key = keyResult.value().stringValue;

            skipWhitespace();
            if (!match(':')) {
                return Result<JsonValue, ConfigError>::error(
                    ConfigError(ConfigError::Code::ParseError,
                               "Expected ':' after key"));
            }

            auto valueResult = parseValue();
            if (valueResult.isError()) {
                return valueResult;
            }
            value.objectValue[key] = std::move(valueResult.value());

            skipWhitespace();
            if (match('}')) break;
            if (!match(',')) {
                return Result<JsonValue, ConfigError>::error(
                    ConfigError(ConfigError::Code::ParseError,
                               "Expected ',' or '}' in object"));
            }
        }

        return Result<JsonValue, ConfigError>::success(std::move(value));
    }
};

// =============================================================================
// Simple YAML Parser (Minimal implementation for configuration)
// =============================================================================

class YamlParser {
public:
    explicit YamlParser(const std::string& input) : input_(input) {}

    Result<JsonValue, ConfigError> parse() {
        lines_.clear();
        std::istringstream stream(input_);
        std::string line;
        while (std::getline(stream, line)) {
            lines_.push_back(line);
        }

        try {
            JsonValue root;
            root.type = JsonType::Object;
            parseObject(root.objectValue, 0, 0, lines_.size());
            return Result<JsonValue, ConfigError>::success(std::move(root));
        } catch (const std::exception& e) {
            return Result<JsonValue, ConfigError>::error(
                ConfigError(ConfigError::Code::ParseError, e.what()));
        }
    }

private:
    std::string input_;
    std::vector<std::string> lines_;

    size_t getIndent(const std::string& line) const {
        size_t indent = 0;
        for (char c : line) {
            if (c == ' ') indent++;
            else break;
        }
        return indent;
    }

    std::string trim(const std::string& s) const {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
        return s.substr(start, end - start);
    }

    bool isBlankOrComment(const std::string& line) const {
        std::string trimmed = trim(line);
        return trimmed.empty() || trimmed[0] == '#';
    }

    void parseObject(std::map<std::string, JsonValue>& obj, size_t baseIndent,
                     size_t startLine, size_t endLine) {
        size_t i = startLine;
        while (i < endLine) {
            if (isBlankOrComment(lines_[i])) {
                i++;
                continue;
            }

            size_t indent = getIndent(lines_[i]);
            if (indent < baseIndent) {
                break;
            }

            std::string line = trim(lines_[i]);
            size_t colonPos = line.find(':');
            if (colonPos == std::string::npos) {
                i++;
                continue;
            }

            std::string key = trim(line.substr(0, colonPos));
            std::string valueStr = trim(line.substr(colonPos + 1));

            if (valueStr.empty()) {
                // Nested object - find extent
                size_t nestedEnd = i + 1;
                while (nestedEnd < endLine) {
                    if (isBlankOrComment(lines_[nestedEnd])) {
                        nestedEnd++;
                        continue;
                    }
                    if (getIndent(lines_[nestedEnd]) <= indent) {
                        break;
                    }
                    nestedEnd++;
                }

                JsonValue nested;
                nested.type = JsonType::Object;
                parseObject(nested.objectValue, indent + 2, i + 1, nestedEnd);
                obj[key] = std::move(nested);
                i = nestedEnd;
            } else {
                obj[key] = parseScalar(valueStr);
                i++;
            }
        }
    }

    JsonValue parseScalar(const std::string& value) {
        JsonValue result;

        // Remove quotes if present
        std::string cleanValue = value;
        if (cleanValue.size() >= 2 &&
            ((cleanValue.front() == '"' && cleanValue.back() == '"') ||
             (cleanValue.front() == '\'' && cleanValue.back() == '\''))) {
            cleanValue = cleanValue.substr(1, cleanValue.size() - 2);
        }

        // Check for boolean
        if (cleanValue == "true" || cleanValue == "True" || cleanValue == "TRUE") {
            result.type = JsonType::Boolean;
            result.boolValue = true;
            return result;
        }
        if (cleanValue == "false" || cleanValue == "False" || cleanValue == "FALSE") {
            result.type = JsonType::Boolean;
            result.boolValue = false;
            return result;
        }

        // Check for null
        if (cleanValue == "null" || cleanValue == "~" || cleanValue.empty()) {
            result.type = JsonType::Null;
            return result;
        }

        // Check for number
        try {
            size_t pos = 0;
            double num = std::stod(cleanValue, &pos);
            if (pos == cleanValue.size()) {
                result.type = JsonType::Number;
                result.numberValue = num;
                return result;
            }
        } catch (...) {
            // Not a number, treat as string
        }

        // Default to string
        result.type = JsonType::String;
        result.stringValue = cleanValue;
        return result;
    }
};

} // anonymous namespace

// =============================================================================
// ConfigManager Implementation
// =============================================================================

ConfigManager::ConfigManager() {
    // Initialize with defaults without logging
    config_ = Configuration{};
}

ConfigManager::~ConfigManager() = default;

Result<void, ConfigError> ConfigManager::loadFromFile(const std::string& filePath) {
    // Read file content
    auto contentResult = readFile(filePath);
    if (contentResult.isError()) {
        return Result<void, ConfigError>::error(contentResult.error());
    }

    // Detect format and parse
    ConfigFormat format = detectFormat(filePath);
    if (static_cast<int>(format) == -1) {
        return Result<void, ConfigError>::error(
            ConfigError(ConfigError::Code::UnsupportedFormat,
                       "Unsupported configuration file format. Use .json, .yaml, or .yml"));
    }

    switch (format) {
        case ConfigFormat::JSON:
            return loadFromJsonString(contentResult.value());
        case ConfigFormat::YAML:
            return loadFromYamlString(contentResult.value());
        default:
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::UnsupportedFormat,
                           "Unsupported configuration file format"));
    }
}

Result<void, ConfigError> ConfigManager::loadFromJsonString(const std::string& jsonContent) {
    auto result = parseJson(jsonContent);
    if (result.isSuccess()) {
        logEffectiveConfig();
    }
    return result;
}

Result<void, ConfigError> ConfigManager::loadFromYamlString(const std::string& yamlContent) {
    auto result = parseYaml(yamlContent);
    if (result.isSuccess()) {
        logEffectiveConfig();
    }
    return result;
}

Result<void, ConfigError> ConfigManager::loadDefaults() {
    std::unique_lock<std::shared_mutex> lock(configMutex_);

    // Reset to default configuration
    config_ = Configuration{};

    // Apply platform-specific defaults
    if (isMobile_) {
        config_.server.maxConnections = 100;
        config_.server.maxPublishers = 3;
        config_.streaming.maxPublishers = 3;
        config_.streaming.maxSubscribersPerStream = 50;
    } else {
        config_.server.maxConnections = 1000;
        config_.server.maxPublishers = 10;
        config_.streaming.maxPublishers = 10;
        config_.streaming.maxSubscribersPerStream = 100;
    }

    lock.unlock();
    log("Configuration loaded with default values");
    logEffectiveConfig();

    return Result<void, ConfigError>::success();
}

void ConfigManager::applyEnvironmentOverrides() {
    std::unique_lock<std::shared_mutex> lock(configMutex_);

    // Server settings
    if (auto val = getEnvVar("OPENRTMP_PORT")) {
        try {
            config_.server.port = static_cast<uint16_t>(std::stoi(*val));
            log("Environment override: OPENRTMP_PORT=" + *val);
        } catch (...) {
            log("Warning: Invalid OPENRTMP_PORT value: " + *val);
        }
    }

    if (auto val = getEnvVar("OPENRTMP_RTMPS_PORT")) {
        try {
            config_.server.rtmpsPort = static_cast<uint16_t>(std::stoi(*val));
            log("Environment override: OPENRTMP_RTMPS_PORT=" + *val);
        } catch (...) {
            log("Warning: Invalid OPENRTMP_RTMPS_PORT value: " + *val);
        }
    }

    if (auto val = getEnvVar("OPENRTMP_BIND_ADDRESS")) {
        config_.server.bindAddress = *val;
        log("Environment override: OPENRTMP_BIND_ADDRESS=" + *val);
    }

    if (auto val = getEnvVar("OPENRTMP_MAX_CONNECTIONS")) {
        try {
            config_.server.maxConnections = static_cast<uint32_t>(std::stoul(*val));
            log("Environment override: OPENRTMP_MAX_CONNECTIONS=" + *val);
        } catch (...) {
            log("Warning: Invalid OPENRTMP_MAX_CONNECTIONS value: " + *val);
        }
    }

    // Logging settings
    if (auto val = getEnvVar("OPENRTMP_LOG_LEVEL")) {
        std::string lower = *val;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (auto level = parseLogLevel(lower)) {
            config_.logging.level = *level;
            log("Environment override: OPENRTMP_LOG_LEVEL=" + *val);
        } else {
            log("Warning: Invalid OPENRTMP_LOG_LEVEL value: " + *val);
        }
    }

    // TLS settings
    if (auto val = getEnvVar("OPENRTMP_TLS_ENABLED")) {
        std::string lower = *val;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        config_.tls.enabled = (lower == "true" || lower == "1" || lower == "yes");
        log("Environment override: OPENRTMP_TLS_ENABLED=" + *val);
    }

    if (auto val = getEnvVar("OPENRTMP_TLS_CERT_PATH")) {
        config_.tls.certPath = *val;
        log("Environment override: OPENRTMP_TLS_CERT_PATH=" + *val);
    }

    if (auto val = getEnvVar("OPENRTMP_TLS_KEY_PATH")) {
        config_.tls.keyPath = *val;
        log("Environment override: OPENRTMP_TLS_KEY_PATH=" + *val);
    }

    // Auth settings
    if (auto val = getEnvVar("OPENRTMP_AUTH_PUBLISH_ENABLED")) {
        std::string lower = *val;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        config_.auth.publishAuthEnabled = (lower == "true" || lower == "1" || lower == "yes");
        log("Environment override: OPENRTMP_AUTH_PUBLISH_ENABLED=" + *val);
    }

    if (auto val = getEnvVar("OPENRTMP_AUTH_SUBSCRIBE_ENABLED")) {
        std::string lower = *val;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        config_.auth.subscribeAuthEnabled = (lower == "true" || lower == "1" || lower == "yes");
        log("Environment override: OPENRTMP_AUTH_SUBSCRIBE_ENABLED=" + *val);
    }
}

Result<void, ConfigError> ConfigManager::validate() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);

    // Validate port ranges
    if (config_.server.port == 0) {
        return Result<void, ConfigError>::error(
            ConfigError(ConfigError::Code::ValidationError,
                       "server.port must be between 1 and 65535",
                       "server.port"));
    }

    // Validate maxConnections
    if (config_.server.maxConnections == 0) {
        return Result<void, ConfigError>::error(
            ConfigError(ConfigError::Code::ValidationError,
                       "server.maxConnections must be greater than 0",
                       "server.maxConnections"));
    }

    // Validate TLS settings
    if (config_.tls.enabled) {
        if (config_.tls.certPath.empty()) {
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::ValidationError,
                           "tls.certPath is required when TLS is enabled",
                           "tls.certPath"));
        }
        if (config_.tls.keyPath.empty()) {
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::ValidationError,
                           "tls.keyPath is required when TLS is enabled",
                           "tls.keyPath"));
        }
    }

    // Validate streaming settings
    if (config_.streaming.gopBufferSeconds < 0) {
        return Result<void, ConfigError>::error(
            ConfigError(ConfigError::Code::ValidationError,
                       "streaming.gopBufferSeconds must be >= 0",
                       "streaming.gopBufferSeconds"));
    }

    return Result<void, ConfigError>::success();
}

const Configuration& ConfigManager::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return config_;
}

std::string ConfigManager::dumpConfig(ConfigFormat format) const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);

    std::ostringstream ss;

    if (format == ConfigFormat::JSON) {
        ss << "{\n";
        ss << "  \"server\": {\n";
        ss << "    \"port\": " << config_.server.port << ",\n";
        ss << "    \"rtmpsPort\": " << config_.server.rtmpsPort << ",\n";
        ss << "    \"bindAddress\": \"" << config_.server.bindAddress << "\",\n";
        ss << "    \"maxConnections\": " << config_.server.maxConnections << "\n";
        ss << "  },\n";
        ss << "  \"logging\": {\n";
        ss << "    \"level\": \"" << logLevelToString(config_.logging.level) << "\",\n";
        ss << "    \"enableConsole\": " << (config_.logging.enableConsole ? "true" : "false") << ",\n";
        ss << "    \"enableFile\": " << (config_.logging.enableFile ? "true" : "false") << "\n";
        ss << "  },\n";
        ss << "  \"auth\": {\n";
        ss << "    \"publishAuthEnabled\": " << (config_.auth.publishAuthEnabled ? "true" : "false") << ",\n";
        ss << "    \"subscribeAuthEnabled\": " << (config_.auth.subscribeAuthEnabled ? "true" : "false") << "\n";
        ss << "  },\n";
        ss << "  \"tls\": {\n";
        ss << "    \"enabled\": " << (config_.tls.enabled ? "true" : "false") << "\n";
        ss << "  }\n";
        ss << "}\n";
    } else {
        ss << "server:\n";
        ss << "  port: " << config_.server.port << "\n";
        ss << "  rtmpsPort: " << config_.server.rtmpsPort << "\n";
        ss << "  bindAddress: \"" << config_.server.bindAddress << "\"\n";
        ss << "  maxConnections: " << config_.server.maxConnections << "\n";
        ss << "logging:\n";
        ss << "  level: " << logLevelToString(config_.logging.level) << "\n";
        ss << "  enableConsole: " << (config_.logging.enableConsole ? "true" : "false") << "\n";
        ss << "  enableFile: " << (config_.logging.enableFile ? "true" : "false") << "\n";
        ss << "auth:\n";
        ss << "  publishAuthEnabled: " << (config_.auth.publishAuthEnabled ? "true" : "false") << "\n";
        ss << "  subscribeAuthEnabled: " << (config_.auth.subscribeAuthEnabled ? "true" : "false") << "\n";
        ss << "tls:\n";
        ss << "  enabled: " << (config_.tls.enabled ? "true" : "false") << "\n";
    }

    return ss.str();
}

void ConfigManager::setMobilePlatform(bool isMobile) {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    isMobile_ = isMobile;
}

bool ConfigManager::isMobilePlatform() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return isMobile_;
}

void ConfigManager::setLogCallback(ConfigLogCallback callback) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logCallback_ = std::move(callback);
}

// =============================================================================
// Private Implementation
// =============================================================================

Result<void, ConfigError> ConfigManager::parseJson(const std::string& content) {
    JsonParser parser(content);
    auto result = parser.parse();

    if (result.isError()) {
        return Result<void, ConfigError>::error(result.error());
    }

    const JsonValue& root = result.value();
    if (!root.isObject()) {
        return Result<void, ConfigError>::error(
            ConfigError(ConfigError::Code::ParseError,
                       "Configuration root must be an object"));
    }

    std::unique_lock<std::shared_mutex> lock(configMutex_);

    // Parse server section
    if (root.contains("server")) {
        const JsonValue& server = root["server"];
        if (server.contains("port")) {
            int64_t port = server["port"].getInt(-1);
            if (port <= 0 || port > 65535) {
                return Result<void, ConfigError>::error(
                    ConfigError(ConfigError::Code::ValidationError,
                               "server.port must be between 1 and 65535",
                               "server.port"));
            }
            config_.server.port = static_cast<uint16_t>(port);
        }
        if (server.contains("rtmpsPort")) {
            config_.server.rtmpsPort = static_cast<uint16_t>(server["rtmpsPort"].getInt(1936));
        }
        if (server.contains("bindAddress")) {
            config_.server.bindAddress = server["bindAddress"].getString("0.0.0.0");
        }
        if (server.contains("maxConnections")) {
            int64_t maxConn = server["maxConnections"].getInt(0);
            if (maxConn <= 0) {
                return Result<void, ConfigError>::error(
                    ConfigError(ConfigError::Code::ValidationError,
                               "server.maxConnections must be greater than 0",
                               "server.maxConnections"));
            }
            config_.server.maxConnections = static_cast<uint32_t>(maxConn);
        }
        if (server.contains("handshakeTimeoutMs")) {
            config_.server.handshakeTimeoutMs = static_cast<uint32_t>(
                server["handshakeTimeoutMs"].getInt(10000));
        }
        if (server.contains("connectionTimeoutMs")) {
            config_.server.connectionTimeoutMs = static_cast<uint32_t>(
                server["connectionTimeoutMs"].getInt(30000));
        }
    }

    // Parse logging section
    if (root.contains("logging")) {
        const JsonValue& logging = root["logging"];
        if (logging.contains("level")) {
            std::string levelStr = logging["level"].getString("info");
            auto level = parseLogLevel(levelStr);
            if (!level) {
                return Result<void, ConfigError>::error(
                    ConfigError(ConfigError::Code::ValidationError,
                               "Invalid logging.level: " + levelStr + ". Valid values: debug, info, warning, error",
                               "logging.level"));
            }
            config_.logging.level = *level;
        }
        if (logging.contains("enableConsole")) {
            config_.logging.enableConsole = logging["enableConsole"].getBool(false);
        }
        if (logging.contains("enableFile")) {
            config_.logging.enableFile = logging["enableFile"].getBool(false);
        }
        if (logging.contains("filePath")) {
            config_.logging.filePath = logging["filePath"].getString();
        }
    }

    // Parse auth section
    if (root.contains("auth")) {
        const JsonValue& auth = root["auth"];
        if (auth.contains("publishAuthEnabled")) {
            config_.auth.publishAuthEnabled = auth["publishAuthEnabled"].getBool(true);
        }
        if (auth.contains("subscribeAuthEnabled")) {
            config_.auth.subscribeAuthEnabled = auth["subscribeAuthEnabled"].getBool(false);
        }
        if (auth.contains("streamKeys") && auth["streamKeys"].isArray()) {
            config_.auth.streamKeys.clear();
            for (const auto& key : auth["streamKeys"].arrayValue) {
                if (key.isString()) {
                    config_.auth.streamKeys.push_back(key.stringValue);
                }
            }
        }
    }

    // Parse TLS section
    if (root.contains("tls")) {
        const JsonValue& tls = root["tls"];
        if (tls.contains("enabled")) {
            config_.tls.enabled = tls["enabled"].getBool(false);
        }
        if (tls.contains("certPath")) {
            config_.tls.certPath = tls["certPath"].getString();
        }
        if (tls.contains("keyPath")) {
            config_.tls.keyPath = tls["keyPath"].getString();
        }
        if (tls.contains("minVersion")) {
            config_.tls.minVersion = tls["minVersion"].getString("TLS1.2");
        }
    }

    // Parse streaming section
    if (root.contains("streaming")) {
        const JsonValue& streaming = root["streaming"];
        if (streaming.contains("gopBufferSeconds")) {
            double gopBuffer = streaming["gopBufferSeconds"].getDouble(2.0);
            if (gopBuffer < 0) {
                return Result<void, ConfigError>::error(
                    ConfigError(ConfigError::Code::ValidationError,
                               "streaming.gopBufferSeconds must be >= 0",
                               "streaming.gopBufferSeconds"));
            }
            config_.streaming.gopBufferSeconds = gopBuffer;
        }
        if (streaming.contains("maxSubscriberBuffer")) {
            config_.streaming.maxSubscriberBufferMs = static_cast<uint32_t>(
                streaming["maxSubscriberBuffer"].getInt(5000));
        }
        if (streaming.contains("lowLatencyMode")) {
            config_.streaming.lowLatencyMode = streaming["lowLatencyMode"].getBool(false);
        }
    }

    lock.unlock();

    // Validate after parsing
    return validate();
}

Result<void, ConfigError> ConfigManager::parseYaml(const std::string& content) {
    YamlParser parser(content);
    auto result = parser.parse();

    if (result.isError()) {
        return Result<void, ConfigError>::error(result.error());
    }

    const JsonValue& root = result.value();
    if (!root.isObject()) {
        return Result<void, ConfigError>::error(
            ConfigError(ConfigError::Code::ParseError,
                       "Configuration root must be an object"));
    }

    std::unique_lock<std::shared_mutex> lock(configMutex_);

    // Parse server section
    if (root.contains("server")) {
        const JsonValue& server = root["server"];
        if (server.contains("port")) {
            int64_t port = server["port"].getInt(-1);
            if (port <= 0 || port > 65535) {
                return Result<void, ConfigError>::error(
                    ConfigError(ConfigError::Code::ValidationError,
                               "server.port must be between 1 and 65535",
                               "server.port"));
            }
            config_.server.port = static_cast<uint16_t>(port);
        }
        if (server.contains("rtmpsPort")) {
            config_.server.rtmpsPort = static_cast<uint16_t>(server["rtmpsPort"].getInt(1936));
        }
        if (server.contains("bindAddress")) {
            config_.server.bindAddress = server["bindAddress"].getString("0.0.0.0");
        }
        if (server.contains("maxConnections")) {
            int64_t maxConn = server["maxConnections"].getInt(0);
            if (maxConn <= 0) {
                return Result<void, ConfigError>::error(
                    ConfigError(ConfigError::Code::ValidationError,
                               "server.maxConnections must be greater than 0",
                               "server.maxConnections"));
            }
            config_.server.maxConnections = static_cast<uint32_t>(maxConn);
        }
        if (server.contains("handshakeTimeoutMs")) {
            config_.server.handshakeTimeoutMs = static_cast<uint32_t>(
                server["handshakeTimeoutMs"].getInt(10000));
        }
        if (server.contains("connectionTimeoutMs")) {
            config_.server.connectionTimeoutMs = static_cast<uint32_t>(
                server["connectionTimeoutMs"].getInt(30000));
        }
    }

    // Parse logging section
    if (root.contains("logging")) {
        const JsonValue& logging = root["logging"];
        if (logging.contains("level")) {
            std::string levelStr = logging["level"].getString("info");
            auto level = parseLogLevel(levelStr);
            if (!level) {
                return Result<void, ConfigError>::error(
                    ConfigError(ConfigError::Code::ValidationError,
                               "Invalid logging.level: " + levelStr + ". Valid values: debug, info, warning, error",
                               "logging.level"));
            }
            config_.logging.level = *level;
        }
        if (logging.contains("enableConsole")) {
            config_.logging.enableConsole = logging["enableConsole"].getBool(false);
        }
        if (logging.contains("enableFile")) {
            config_.logging.enableFile = logging["enableFile"].getBool(false);
        }
    }

    // Parse auth section
    if (root.contains("auth")) {
        const JsonValue& auth = root["auth"];
        if (auth.contains("publishAuthEnabled")) {
            config_.auth.publishAuthEnabled = auth["publishAuthEnabled"].getBool(true);
        }
        if (auth.contains("subscribeAuthEnabled")) {
            config_.auth.subscribeAuthEnabled = auth["subscribeAuthEnabled"].getBool(false);
        }
    }

    // Parse TLS section
    if (root.contains("tls")) {
        const JsonValue& tls = root["tls"];
        if (tls.contains("enabled")) {
            config_.tls.enabled = tls["enabled"].getBool(false);
        }
        if (tls.contains("certPath")) {
            config_.tls.certPath = tls["certPath"].getString();
        }
        if (tls.contains("keyPath")) {
            config_.tls.keyPath = tls["keyPath"].getString();
        }
    }

    lock.unlock();

    // Validate after parsing
    return validate();
}

ConfigFormat ConfigManager::detectFormat(const std::string& filePath) const {
    // Find the file extension
    size_t dotPos = filePath.rfind('.');
    if (dotPos == std::string::npos) {
        return ConfigFormat::JSON; // Default
    }

    std::string ext = filePath.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".json") return ConfigFormat::JSON;
    if (ext == ".yaml" || ext == ".yml") return ConfigFormat::YAML;

    // Unsupported - return invalid format
    return static_cast<ConfigFormat>(-1);
}

Result<std::string, ConfigError> ConfigManager::readFile(const std::string& filePath) const {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return Result<std::string, ConfigError>::error(
            ConfigError(ConfigError::Code::FileNotFound,
                       "Configuration file not found: " + filePath));
    }

    std::ostringstream ss;
    ss << file.rdbuf();

    if (file.fail() && !file.eof()) {
        return Result<std::string, ConfigError>::error(
            ConfigError(ConfigError::Code::IOError,
                       "Error reading configuration file: " + filePath));
    }

    return Result<std::string, ConfigError>::success(ss.str());
}

std::optional<LogLevel> ConfigManager::parseLogLevel(const std::string& level) const {
    return stringToLogLevel(level);
}

void ConfigManager::log(const std::string& message) const {
    std::lock_guard<std::mutex> lock(logMutex_);
    if (logCallback_) {
        logCallback_(message);
    }
}

void ConfigManager::logEffectiveConfig() const {
    std::shared_lock<std::shared_mutex> configLock(configMutex_);
    log("Effective configuration:");
    log("  server.port: " + std::to_string(config_.server.port));
    log("  server.rtmpsPort: " + std::to_string(config_.server.rtmpsPort));
    log("  server.bindAddress: " + config_.server.bindAddress);
    log("  server.maxConnections: " + std::to_string(config_.server.maxConnections));
    log("  logging.level: " + logLevelToString(config_.logging.level));
    log("  auth.publishAuthEnabled: " + std::string(config_.auth.publishAuthEnabled ? "true" : "false"));
    log("  auth.subscribeAuthEnabled: " + std::string(config_.auth.subscribeAuthEnabled ? "true" : "false"));
    log("  tls.enabled: " + std::string(config_.tls.enabled ? "true" : "false"));
}

std::optional<std::string> ConfigManager::getEnvVar(const std::string& name) const {
    const char* value = std::getenv(name.c_str());
    if (value != nullptr) {
        return std::string(value);
    }
    return std::nullopt;
}

// =============================================================================
// Runtime Configuration Implementation (Task 12.2)
// =============================================================================

Result<void, ConfigError> ConfigManager::applyRuntimeUpdate(const RuntimeConfigUpdate& update) {
    // Validate the update first
    auto validateResult = validateRuntimeUpdate(update);
    if (validateResult.isError()) {
        return validateResult;
    }

    std::unique_lock<std::shared_mutex> lock(configMutex_);

    // Apply log level
    if (update.logLevel.has_value()) {
        std::string oldValue = logLevelToString(config_.logging.level);
        config_.logging.level = *update.logLevel;
        std::string newValue = logLevelToString(config_.logging.level);
        lock.unlock();
        emitChangeEvent(ConfigParameter::LogLevel, oldValue, newValue);
        lock.lock();
    }

    // Apply console log setting
    if (update.enableConsoleLog.has_value()) {
        std::string oldValue = config_.logging.enableConsole ? "true" : "false";
        config_.logging.enableConsole = *update.enableConsoleLog;
        std::string newValue = config_.logging.enableConsole ? "true" : "false";
        lock.unlock();
        emitChangeEvent(ConfigParameter::EnableConsoleLog, oldValue, newValue);
        lock.lock();
    }

    // Apply file log setting
    if (update.enableFileLog.has_value()) {
        std::string oldValue = config_.logging.enableFile ? "true" : "false";
        config_.logging.enableFile = *update.enableFileLog;
        std::string newValue = config_.logging.enableFile ? "true" : "false";
        lock.unlock();
        emitChangeEvent(ConfigParameter::EnableFileLog, oldValue, newValue);
        lock.lock();
    }

    // Apply GOP buffer seconds
    if (update.gopBufferSeconds.has_value()) {
        std::string oldValue = std::to_string(config_.streaming.gopBufferSeconds);
        config_.streaming.gopBufferSeconds = *update.gopBufferSeconds;
        std::string newValue = std::to_string(config_.streaming.gopBufferSeconds);
        lock.unlock();
        emitChangeEvent(ConfigParameter::GopBufferSeconds, oldValue, newValue);
        lock.lock();
    }

    // Apply max subscriber buffer
    if (update.maxSubscriberBufferMs.has_value()) {
        std::string oldValue = std::to_string(config_.streaming.maxSubscriberBufferMs);
        config_.streaming.maxSubscriberBufferMs = *update.maxSubscriberBufferMs;
        std::string newValue = std::to_string(config_.streaming.maxSubscriberBufferMs);
        lock.unlock();
        emitChangeEvent(ConfigParameter::MaxSubscriberBuffer, oldValue, newValue);
        lock.lock();
    }

    // Apply low latency mode
    if (update.lowLatencyMode.has_value()) {
        std::string oldValue = config_.streaming.lowLatencyMode ? "true" : "false";
        config_.streaming.lowLatencyMode = *update.lowLatencyMode;
        std::string newValue = config_.streaming.lowLatencyMode ? "true" : "false";
        lock.unlock();
        emitChangeEvent(ConfigParameter::LowLatencyMode, oldValue, newValue);
        lock.lock();
    }

    // Apply low latency buffer
    if (update.lowLatencyBufferMs.has_value()) {
        std::string oldValue = std::to_string(config_.streaming.lowLatencyBufferMs);
        config_.streaming.lowLatencyBufferMs = *update.lowLatencyBufferMs;
        std::string newValue = std::to_string(config_.streaming.lowLatencyBufferMs);
        lock.unlock();
        emitChangeEvent(ConfigParameter::LowLatencyBuffer, oldValue, newValue);
        lock.lock();
    }

    // Apply handshake timeout
    if (update.handshakeTimeoutMs.has_value()) {
        std::string oldValue = std::to_string(config_.server.handshakeTimeoutMs);
        config_.server.handshakeTimeoutMs = *update.handshakeTimeoutMs;
        std::string newValue = std::to_string(config_.server.handshakeTimeoutMs);
        lock.unlock();
        emitChangeEvent(ConfigParameter::HandshakeTimeout, oldValue, newValue);
        lock.lock();
    }

    // Apply connection timeout
    if (update.connectionTimeoutMs.has_value()) {
        std::string oldValue = std::to_string(config_.server.connectionTimeoutMs);
        config_.server.connectionTimeoutMs = *update.connectionTimeoutMs;
        std::string newValue = std::to_string(config_.server.connectionTimeoutMs);
        lock.unlock();
        emitChangeEvent(ConfigParameter::ConnectionTimeout, oldValue, newValue);
        lock.lock();
    }

    // Apply publish auth enabled
    if (update.publishAuthEnabled.has_value()) {
        std::string oldValue = config_.auth.publishAuthEnabled ? "true" : "false";
        config_.auth.publishAuthEnabled = *update.publishAuthEnabled;
        std::string newValue = config_.auth.publishAuthEnabled ? "true" : "false";
        lock.unlock();
        emitChangeEvent(ConfigParameter::PublishAuthEnabled, oldValue, newValue);
        lock.lock();
    }

    // Apply subscribe auth enabled
    if (update.subscribeAuthEnabled.has_value()) {
        std::string oldValue = config_.auth.subscribeAuthEnabled ? "true" : "false";
        config_.auth.subscribeAuthEnabled = *update.subscribeAuthEnabled;
        std::string newValue = config_.auth.subscribeAuthEnabled ? "true" : "false";
        lock.unlock();
        emitChangeEvent(ConfigParameter::SubscribeAuthEnabled, oldValue, newValue);
        lock.lock();
    }

    // Apply auth timeout
    if (update.authTimeoutMs.has_value()) {
        std::string oldValue = std::to_string(config_.auth.authTimeoutMs);
        config_.auth.authTimeoutMs = *update.authTimeoutMs;
        std::string newValue = std::to_string(config_.auth.authTimeoutMs);
        lock.unlock();
        emitChangeEvent(ConfigParameter::AuthTimeout, oldValue, newValue);
        lock.lock();
    }

    // Apply rate limit
    if (update.rateLimitPerMinute.has_value()) {
        std::string oldValue = std::to_string(config_.auth.rateLimitPerMinute);
        config_.auth.rateLimitPerMinute = *update.rateLimitPerMinute;
        std::string newValue = std::to_string(config_.auth.rateLimitPerMinute);
        lock.unlock();
        emitChangeEvent(ConfigParameter::RateLimitPerMinute, oldValue, newValue);
        lock.lock();
    }

    lock.unlock();
    log("Runtime configuration update applied successfully");

    return Result<void, ConfigError>::success();
}

Result<void, ConfigError> ConfigManager::validateRuntimeUpdate(const RuntimeConfigUpdate& update) const {
    // Validate GOP buffer seconds
    if (update.gopBufferSeconds.has_value()) {
        if (*update.gopBufferSeconds < 0) {
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::ValidationError,
                           "gopBufferSeconds must be >= 0",
                           "streaming.gopBufferSeconds"));
        }
    }

    // Validate max subscriber buffer
    if (update.maxSubscriberBufferMs.has_value()) {
        if (*update.maxSubscriberBufferMs == 0) {
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::ValidationError,
                           "maxSubscriberBufferMs must be > 0",
                           "streaming.maxSubscriberBufferMs"));
        }
    }

    // Validate low latency buffer
    if (update.lowLatencyBufferMs.has_value()) {
        if (*update.lowLatencyBufferMs == 0) {
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::ValidationError,
                           "lowLatencyBufferMs must be > 0",
                           "streaming.lowLatencyBufferMs"));
        }
    }

    // Validate handshake timeout
    if (update.handshakeTimeoutMs.has_value()) {
        if (*update.handshakeTimeoutMs == 0) {
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::ValidationError,
                           "handshakeTimeoutMs must be > 0",
                           "server.handshakeTimeoutMs"));
        }
    }

    // Validate connection timeout
    if (update.connectionTimeoutMs.has_value()) {
        if (*update.connectionTimeoutMs == 0) {
            return Result<void, ConfigError>::error(
                ConfigError(ConfigError::Code::ValidationError,
                           "connectionTimeoutMs must be > 0",
                           "server.connectionTimeoutMs"));
        }
    }

    return Result<void, ConfigError>::success();
}

Result<HotReloadResult, ConfigError> ConfigManager::hotReloadFromFile(const std::string& filePath) {
    // Read the new configuration file
    auto contentResult = readFile(filePath);
    if (contentResult.isError()) {
        return Result<HotReloadResult, ConfigError>::error(contentResult.error());
    }

    // Detect format and parse into temporary config
    ConfigFormat format = detectFormat(filePath);
    Configuration newConfig;

    // Save current config to compare
    Configuration currentConfig;
    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        currentConfig = config_;
        newConfig = config_;  // Start with current config as base
    }

    // Parse the new config file into a temporary manager to extract values
    ConfigManager tempManager;
    if (format == ConfigFormat::JSON) {
        auto parseResult = tempManager.loadFromJsonString(contentResult.value());
        if (parseResult.isError()) {
            return Result<HotReloadResult, ConfigError>::error(parseResult.error());
        }
    } else {
        auto parseResult = tempManager.loadFromYamlString(contentResult.value());
        if (parseResult.isError()) {
            return Result<HotReloadResult, ConfigError>::error(parseResult.error());
        }
    }

    const Configuration& parsedConfig = tempManager.getConfig();

    HotReloadResult result;

    // Check each parameter and apply only hot-reloadable ones
    // Server parameters that require restart
    if (parsedConfig.server.port != currentConfig.server.port) {
        result.skippedChanges.push_back(ConfigParameter::ServerPort);
    }
    if (parsedConfig.server.rtmpsPort != currentConfig.server.rtmpsPort) {
        result.skippedChanges.push_back(ConfigParameter::RtmpsPort);
    }
    if (parsedConfig.server.bindAddress != currentConfig.server.bindAddress) {
        result.skippedChanges.push_back(ConfigParameter::BindAddress);
    }
    if (parsedConfig.server.maxConnections != currentConfig.server.maxConnections) {
        result.skippedChanges.push_back(ConfigParameter::MaxConnections);
    }

    // TLS parameters that require restart
    if (parsedConfig.tls.enabled != currentConfig.tls.enabled) {
        result.skippedChanges.push_back(ConfigParameter::TlsEnabled);
    }
    if (parsedConfig.tls.certPath != currentConfig.tls.certPath) {
        result.skippedChanges.push_back(ConfigParameter::TlsCertPath);
    }
    if (parsedConfig.tls.keyPath != currentConfig.tls.keyPath) {
        result.skippedChanges.push_back(ConfigParameter::TlsKeyPath);
    }

    // Build runtime update with hot-reloadable changes
    RuntimeConfigUpdate runtimeUpdate;

    // Logging (hot-reloadable)
    if (parsedConfig.logging.level != currentConfig.logging.level) {
        runtimeUpdate.logLevel = parsedConfig.logging.level;
        result.appliedChanges.push_back(ConfigParameter::LogLevel);
    }
    if (parsedConfig.logging.enableConsole != currentConfig.logging.enableConsole) {
        runtimeUpdate.enableConsoleLog = parsedConfig.logging.enableConsole;
        result.appliedChanges.push_back(ConfigParameter::EnableConsoleLog);
    }
    if (parsedConfig.logging.enableFile != currentConfig.logging.enableFile) {
        runtimeUpdate.enableFileLog = parsedConfig.logging.enableFile;
        result.appliedChanges.push_back(ConfigParameter::EnableFileLog);
    }

    // Streaming (hot-reloadable)
    if (parsedConfig.streaming.gopBufferSeconds != currentConfig.streaming.gopBufferSeconds) {
        runtimeUpdate.gopBufferSeconds = parsedConfig.streaming.gopBufferSeconds;
        result.appliedChanges.push_back(ConfigParameter::GopBufferSeconds);
    }
    if (parsedConfig.streaming.maxSubscriberBufferMs != currentConfig.streaming.maxSubscriberBufferMs) {
        runtimeUpdate.maxSubscriberBufferMs = parsedConfig.streaming.maxSubscriberBufferMs;
        result.appliedChanges.push_back(ConfigParameter::MaxSubscriberBuffer);
    }
    if (parsedConfig.streaming.lowLatencyMode != currentConfig.streaming.lowLatencyMode) {
        runtimeUpdate.lowLatencyMode = parsedConfig.streaming.lowLatencyMode;
        result.appliedChanges.push_back(ConfigParameter::LowLatencyMode);
    }
    if (parsedConfig.streaming.lowLatencyBufferMs != currentConfig.streaming.lowLatencyBufferMs) {
        runtimeUpdate.lowLatencyBufferMs = parsedConfig.streaming.lowLatencyBufferMs;
        result.appliedChanges.push_back(ConfigParameter::LowLatencyBuffer);
    }

    // Timeouts (hot-reloadable)
    if (parsedConfig.server.handshakeTimeoutMs != currentConfig.server.handshakeTimeoutMs) {
        runtimeUpdate.handshakeTimeoutMs = parsedConfig.server.handshakeTimeoutMs;
        result.appliedChanges.push_back(ConfigParameter::HandshakeTimeout);
    }
    if (parsedConfig.server.connectionTimeoutMs != currentConfig.server.connectionTimeoutMs) {
        runtimeUpdate.connectionTimeoutMs = parsedConfig.server.connectionTimeoutMs;
        result.appliedChanges.push_back(ConfigParameter::ConnectionTimeout);
    }

    // Auth (hot-reloadable)
    if (parsedConfig.auth.publishAuthEnabled != currentConfig.auth.publishAuthEnabled) {
        runtimeUpdate.publishAuthEnabled = parsedConfig.auth.publishAuthEnabled;
        result.appliedChanges.push_back(ConfigParameter::PublishAuthEnabled);
    }
    if (parsedConfig.auth.subscribeAuthEnabled != currentConfig.auth.subscribeAuthEnabled) {
        runtimeUpdate.subscribeAuthEnabled = parsedConfig.auth.subscribeAuthEnabled;
        result.appliedChanges.push_back(ConfigParameter::SubscribeAuthEnabled);
    }

    // Apply the runtime update (this will also emit events)
    // Note: We clear appliedChanges since applyRuntimeUpdate will emit its own events
    auto applyResult = applyRuntimeUpdate(runtimeUpdate);
    if (applyResult.isError()) {
        return Result<HotReloadResult, ConfigError>::error(applyResult.error());
    }

    // Log the reload result
    std::ostringstream ss;
    ss << "Hot-reload completed: " << result.appliedChanges.size() << " changes applied, "
       << result.skippedChanges.size() << " changes require restart";
    log(ss.str());

    return Result<HotReloadResult, ConfigError>::success(std::move(result));
}

bool ConfigManager::requiresRestart(ConfigParameter param) const {
    switch (param) {
        // Server parameters require restart
        case ConfigParameter::ServerPort:
        case ConfigParameter::RtmpsPort:
        case ConfigParameter::BindAddress:
        case ConfigParameter::MaxConnections:
        case ConfigParameter::MaxPublishers:
        // TLS parameters require restart
        case ConfigParameter::TlsEnabled:
        case ConfigParameter::TlsCertPath:
        case ConfigParameter::TlsKeyPath:
            return true;

        // All other parameters are hot-reloadable
        default:
            return false;
    }
}

std::vector<ConfigParameter> ConfigManager::getHotReloadableParameters() const {
    return {
        // Logging
        ConfigParameter::LogLevel,
        ConfigParameter::EnableConsoleLog,
        ConfigParameter::EnableFileLog,
        ConfigParameter::LogFilePath,
        // Streaming
        ConfigParameter::GopBufferSeconds,
        ConfigParameter::MaxSubscriberBuffer,
        ConfigParameter::LowLatencyMode,
        ConfigParameter::LowLatencyBuffer,
        ConfigParameter::MaxBitrate,
        // Timeouts
        ConfigParameter::HandshakeTimeout,
        ConfigParameter::ConnectionTimeout,
        // Auth
        ConfigParameter::PublishAuthEnabled,
        ConfigParameter::SubscribeAuthEnabled,
        ConfigParameter::AuthTimeout,
        ConfigParameter::RateLimitPerMinute
    };
}

std::map<ConfigParameter, ParameterInfo> ConfigManager::getParameterInfo() const {
    return {
        // Server parameters (require restart)
        {ConfigParameter::ServerPort, {"server.port", "RTMP server port", true}},
        {ConfigParameter::RtmpsPort, {"server.rtmpsPort", "RTMPS server port", true}},
        {ConfigParameter::BindAddress, {"server.bindAddress", "Network interface to bind to", true}},
        {ConfigParameter::MaxConnections, {"server.maxConnections", "Maximum concurrent connections", true}},
        {ConfigParameter::MaxPublishers, {"server.maxPublishers", "Maximum concurrent publishers", true}},

        // Logging parameters (hot-reloadable)
        {ConfigParameter::LogLevel, {"logging.level", "Log verbosity level", false}},
        {ConfigParameter::EnableConsoleLog, {"logging.enableConsole", "Enable console output", false}},
        {ConfigParameter::EnableFileLog, {"logging.enableFile", "Enable file output", false}},
        {ConfigParameter::LogFilePath, {"logging.filePath", "Log file path", false}},

        // Auth parameters (hot-reloadable)
        {ConfigParameter::PublishAuthEnabled, {"auth.publishAuthEnabled", "Require auth for publishing", false}},
        {ConfigParameter::SubscribeAuthEnabled, {"auth.subscribeAuthEnabled", "Require auth for subscribing", false}},
        {ConfigParameter::AuthTimeout, {"auth.authTimeoutMs", "External auth timeout in ms", false}},
        {ConfigParameter::RateLimitPerMinute, {"auth.rateLimitPerMinute", "Failed auth attempts per IP per minute", false}},

        // TLS parameters (require restart)
        {ConfigParameter::TlsEnabled, {"tls.enabled", "Enable RTMPS", true}},
        {ConfigParameter::TlsCertPath, {"tls.certPath", "TLS certificate path", true}},
        {ConfigParameter::TlsKeyPath, {"tls.keyPath", "TLS private key path", true}},

        // Streaming parameters (hot-reloadable)
        {ConfigParameter::GopBufferSeconds, {"streaming.gopBufferSeconds", "GOP buffer duration", false}},
        {ConfigParameter::MaxSubscriberBuffer, {"streaming.maxSubscriberBufferMs", "Max subscriber buffer", false}},
        {ConfigParameter::LowLatencyMode, {"streaming.lowLatencyMode", "Enable low latency mode", false}},
        {ConfigParameter::LowLatencyBuffer, {"streaming.lowLatencyBufferMs", "Low latency buffer size", false}},
        {ConfigParameter::MaxBitrate, {"streaming.maxBitrateMbps", "Max stream bitrate", false}},

        // Timeout parameters (hot-reloadable)
        {ConfigParameter::HandshakeTimeout, {"server.handshakeTimeoutMs", "Handshake timeout in ms", false}},
        {ConfigParameter::ConnectionTimeout, {"server.connectionTimeoutMs", "Connection timeout in ms", false}}
    };
}

void ConfigManager::setConfigChangeCallback(ConfigChangeCallback callback) {
    std::lock_guard<std::mutex> lock(changeCallbackMutex_);
    changeCallback_ = std::move(callback);
}

void ConfigManager::emitChangeEvent(ConfigParameter param,
                                    const std::string& oldValue,
                                    const std::string& newValue) {
    std::lock_guard<std::mutex> lock(changeCallbackMutex_);
    if (changeCallback_) {
        ConfigChangeEvent event;
        event.parameter = param;
        event.oldValue = oldValue;
        event.newValue = newValue;
        event.timestamp = std::chrono::steady_clock::now();
        changeCallback_(event);
    }
}

Result<Configuration, ConfigError> ConfigManager::parseConfigFromFile(const std::string& filePath) const {
    auto contentResult = readFile(filePath);
    if (contentResult.isError()) {
        return Result<Configuration, ConfigError>::error(contentResult.error());
    }

    // This is a placeholder - actual implementation would parse and return config
    return Result<Configuration, ConfigError>::success(Configuration{});
}

} // namespace core
} // namespace openrtmp
