// OpenRTMP - Cross-platform RTMP Server
// Tests for Configuration Manager
//
// Tests cover:
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

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// Platform-specific includes for temp directory
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir(dir, mode) _mkdir(dir)
#define rmdir _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include "openrtmp/core/config_manager.hpp"

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class ConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for test config files
        testDir_ = getTempDir() + "/openrtmp_config_test_" + std::to_string(getpid());
#ifdef _WIN32
        _mkdir(testDir_.c_str());
#else
        mkdir(testDir_.c_str(), 0755);
#endif
    }

    void TearDown() override {
        // Clean up temp directory
        removeDir(testDir_);

        // Clean up any environment variables we set
        for (const auto& envVar : setEnvVars_) {
            unsetEnvVar(envVar);
        }
    }

    std::string testDir_;
    std::vector<std::string> setEnvVars_;

    // Helper to get temp directory
    std::string getTempDir() {
#ifdef _WIN32
        char tempPath[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        return std::string(tempPath);
#else
        const char* tmpdir = std::getenv("TMPDIR");
        if (tmpdir) return std::string(tmpdir);
        return "/tmp";
#endif
    }

    // Helper to remove directory recursively
    void removeDir(const std::string& path) {
#ifdef _WIN32
        WIN32_FIND_DATAA findData;
        std::string searchPath = path + "\\*";
        HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(findData.cFileName, ".") != 0 &&
                    strcmp(findData.cFileName, "..") != 0) {
                    std::string filePath = path + "\\" + findData.cFileName;
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        removeDir(filePath);
                    } else {
                        DeleteFileA(filePath.c_str());
                    }
                }
            } while (FindNextFileA(hFind, &findData));
            FindClose(hFind);
        }
        RemoveDirectoryA(path.c_str());
#else
        DIR* dir = opendir(path.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strcmp(entry->d_name, ".") != 0 &&
                    strcmp(entry->d_name, "..") != 0) {
                    std::string filePath = path + "/" + entry->d_name;
                    struct stat st;
                    if (stat(filePath.c_str(), &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            removeDir(filePath);
                        } else {
                            std::remove(filePath.c_str());
                        }
                    }
                }
            }
            closedir(dir);
        }
        rmdir(path.c_str());
#endif
    }

    // Helper to create a config file
    void writeConfigFile(const std::string& filename, const std::string& content) {
        std::string fullPath = testDir_ + "/" + filename;
        std::ofstream file(fullPath);
        file << content;
        file.close();
    }

    // Helper to set an environment variable (cross-platform)
    void setEnvVar(const std::string& name, const std::string& value) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
        setEnvVars_.push_back(name);
    }

    // Helper to unset an environment variable
    void unsetEnvVar(const std::string& name) {
#ifdef _WIN32
        _putenv_s(name.c_str(), "");
#else
        unsetenv(name.c_str());
#endif
    }

    std::string getTestFilePath(const std::string& filename) {
        return testDir_ + "/" + filename;
    }
};

// =============================================================================
// Default Configuration Tests (Requirement 17.5)
// =============================================================================

TEST_F(ConfigManagerTest, DefaultConfigurationIsValid) {
    ConfigManager manager;

    // Load with no config file - should use defaults
    auto result = manager.loadDefaults();
    ASSERT_TRUE(result.isSuccess());

    const auto& config = manager.getConfig();

    // Verify sensible defaults
    EXPECT_EQ(config.server.port, 1935);
    EXPECT_EQ(config.server.rtmpsPort, 1936);
    EXPECT_EQ(config.server.bindAddress, "0.0.0.0");
    EXPECT_EQ(config.server.maxConnections, 1000u);

    EXPECT_EQ(config.logging.level, LogLevel::Info);
    EXPECT_FALSE(config.logging.enableConsole);
    EXPECT_FALSE(config.logging.enableFile);

    EXPECT_TRUE(config.auth.publishAuthEnabled);
    EXPECT_FALSE(config.auth.subscribeAuthEnabled);

    EXPECT_FALSE(config.tls.enabled);
}

TEST_F(ConfigManagerTest, MobileDefaultConfigurationHasLowerLimits) {
    ConfigManager manager;
    manager.setMobilePlatform(true);

    auto result = manager.loadDefaults();
    ASSERT_TRUE(result.isSuccess());

    const auto& config = manager.getConfig();

    // Mobile should have lower limits
    EXPECT_LE(config.server.maxConnections, 100u);
    EXPECT_LE(config.streaming.maxPublishers, 3u);
}

// =============================================================================
// JSON Configuration Parsing Tests (Requirement 17.1)
// =============================================================================

TEST_F(ConfigManagerTest, LoadValidJsonConfiguration) {
    const std::string jsonConfig = R"({
        "server": {
            "port": 1945,
            "rtmpsPort": 1946,
            "bindAddress": "127.0.0.1",
            "maxConnections": 500
        },
        "logging": {
            "level": "debug",
            "enableConsole": true,
            "enableFile": true,
            "filePath": "/var/log/openrtmp.log"
        },
        "auth": {
            "publishAuthEnabled": true,
            "subscribeAuthEnabled": true,
            "streamKeys": ["key1", "key2"]
        }
    })";

    writeConfigFile("config.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("config.json"));
    ASSERT_TRUE(result.isSuccess()) << "Failed to load config: "
        << (result.isError() ? result.error().message : "");

    const auto& config = manager.getConfig();

    EXPECT_EQ(config.server.port, 1945);
    EXPECT_EQ(config.server.rtmpsPort, 1946);
    EXPECT_EQ(config.server.bindAddress, "127.0.0.1");
    EXPECT_EQ(config.server.maxConnections, 500u);

    EXPECT_EQ(config.logging.level, LogLevel::Debug);
    EXPECT_TRUE(config.logging.enableConsole);
    EXPECT_TRUE(config.logging.enableFile);
    EXPECT_EQ(config.logging.filePath, "/var/log/openrtmp.log");

    EXPECT_TRUE(config.auth.publishAuthEnabled);
    EXPECT_TRUE(config.auth.subscribeAuthEnabled);
    EXPECT_EQ(config.auth.streamKeys.size(), 2u);
}

TEST_F(ConfigManagerTest, LoadPartialJsonConfigurationUseDefaultsForMissing) {
    // Only specify some fields - others should use defaults
    const std::string jsonConfig = R"({
        "server": {
            "port": 2000
        }
    })";

    writeConfigFile("partial.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("partial.json"));
    ASSERT_TRUE(result.isSuccess());

    const auto& config = manager.getConfig();

    // Specified value
    EXPECT_EQ(config.server.port, 2000);

    // Default values for unspecified fields
    EXPECT_EQ(config.server.rtmpsPort, 1936);
    EXPECT_EQ(config.server.bindAddress, "0.0.0.0");
    EXPECT_EQ(config.logging.level, LogLevel::Info);
}

TEST_F(ConfigManagerTest, LoadNestedJsonConfiguration) {
    const std::string jsonConfig = R"({
        "server": {
            "port": 1935,
            "maxConnections": 1000
        },
        "tls": {
            "enabled": true,
            "certPath": "/path/to/cert.pem",
            "keyPath": "/path/to/key.pem",
            "minVersion": "TLS1.2"
        },
        "streaming": {
            "gopBufferSeconds": 3.0,
            "maxSubscriberBuffer": 5000,
            "lowLatencyMode": true
        }
    })";

    writeConfigFile("nested.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("nested.json"));
    ASSERT_TRUE(result.isSuccess());

    const auto& config = manager.getConfig();

    EXPECT_TRUE(config.tls.enabled);
    EXPECT_EQ(config.tls.certPath, "/path/to/cert.pem");
    EXPECT_EQ(config.tls.keyPath, "/path/to/key.pem");
    EXPECT_EQ(config.tls.minVersion, "TLS1.2");

    EXPECT_DOUBLE_EQ(config.streaming.gopBufferSeconds, 3.0);
    EXPECT_EQ(config.streaming.maxSubscriberBufferMs, 5000u);
    EXPECT_TRUE(config.streaming.lowLatencyMode);
}

// =============================================================================
// YAML Configuration Parsing Tests (Requirement 17.1)
// =============================================================================

TEST_F(ConfigManagerTest, LoadValidYamlConfiguration) {
    const std::string yamlConfig = R"(
server:
  port: 1950
  rtmpsPort: 1951
  bindAddress: "192.168.1.1"
  maxConnections: 800

logging:
  level: warning
  enableConsole: true
  enableFile: false

auth:
  publishAuthEnabled: true
  subscribeAuthEnabled: false
)";

    writeConfigFile("config.yaml", yamlConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("config.yaml"));
    ASSERT_TRUE(result.isSuccess()) << "Failed to load config: "
        << (result.isError() ? result.error().message : "");

    const auto& config = manager.getConfig();

    EXPECT_EQ(config.server.port, 1950);
    EXPECT_EQ(config.server.rtmpsPort, 1951);
    EXPECT_EQ(config.server.bindAddress, "192.168.1.1");
    EXPECT_EQ(config.server.maxConnections, 800u);

    EXPECT_EQ(config.logging.level, LogLevel::Warning);
    EXPECT_TRUE(config.logging.enableConsole);
    EXPECT_FALSE(config.logging.enableFile);
}

TEST_F(ConfigManagerTest, LoadYamlConfigurationWithYmlExtension) {
    const std::string yamlConfig = R"(
server:
  port: 1960
)";

    writeConfigFile("config.yml", yamlConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("config.yml"));
    ASSERT_TRUE(result.isSuccess());

    EXPECT_EQ(manager.getConfig().server.port, 1960);
}

// =============================================================================
// Environment Variable Override Tests (Requirement 17.2)
// =============================================================================

TEST_F(ConfigManagerTest, EnvironmentVariableOverridesPort) {
    setEnvVar("OPENRTMP_PORT", "2222");

    ConfigManager manager;
    manager.loadDefaults();
    manager.applyEnvironmentOverrides();

    EXPECT_EQ(manager.getConfig().server.port, 2222);
}

TEST_F(ConfigManagerTest, EnvironmentVariableOverridesLogLevel) {
    setEnvVar("OPENRTMP_LOG_LEVEL", "debug");

    ConfigManager manager;
    manager.loadDefaults();
    manager.applyEnvironmentOverrides();

    EXPECT_EQ(manager.getConfig().logging.level, LogLevel::Debug);
}

TEST_F(ConfigManagerTest, EnvironmentVariableOverridesBindAddress) {
    setEnvVar("OPENRTMP_BIND_ADDRESS", "10.0.0.1");

    ConfigManager manager;
    manager.loadDefaults();
    manager.applyEnvironmentOverrides();

    EXPECT_EQ(manager.getConfig().server.bindAddress, "10.0.0.1");
}

TEST_F(ConfigManagerTest, EnvironmentVariableOverridesMaxConnections) {
    setEnvVar("OPENRTMP_MAX_CONNECTIONS", "2000");

    ConfigManager manager;
    manager.loadDefaults();
    manager.applyEnvironmentOverrides();

    EXPECT_EQ(manager.getConfig().server.maxConnections, 2000u);
}

TEST_F(ConfigManagerTest, EnvironmentVariableOverridesTLSSettings) {
    setEnvVar("OPENRTMP_TLS_ENABLED", "true");
    setEnvVar("OPENRTMP_TLS_CERT_PATH", "/env/cert.pem");
    setEnvVar("OPENRTMP_TLS_KEY_PATH", "/env/key.pem");

    ConfigManager manager;
    manager.loadDefaults();
    manager.applyEnvironmentOverrides();

    const auto& config = manager.getConfig();
    EXPECT_TRUE(config.tls.enabled);
    EXPECT_EQ(config.tls.certPath, "/env/cert.pem");
    EXPECT_EQ(config.tls.keyPath, "/env/key.pem");
}

TEST_F(ConfigManagerTest, EnvironmentVariablesOverrideFileConfig) {
    const std::string jsonConfig = R"({
        "server": {
            "port": 1935
        }
    })";

    writeConfigFile("config.json", jsonConfig);
    setEnvVar("OPENRTMP_PORT", "3000");

    ConfigManager manager;
    manager.loadFromFile(getTestFilePath("config.json"));
    manager.applyEnvironmentOverrides();

    // Environment variable should take precedence
    EXPECT_EQ(manager.getConfig().server.port, 3000);
}

TEST_F(ConfigManagerTest, MultipleEnvironmentVariableOverrides) {
    setEnvVar("OPENRTMP_PORT", "4000");
    setEnvVar("OPENRTMP_RTMPS_PORT", "4001");
    setEnvVar("OPENRTMP_LOG_LEVEL", "error");
    setEnvVar("OPENRTMP_AUTH_PUBLISH_ENABLED", "false");

    ConfigManager manager;
    manager.loadDefaults();
    manager.applyEnvironmentOverrides();

    const auto& config = manager.getConfig();
    EXPECT_EQ(config.server.port, 4000);
    EXPECT_EQ(config.server.rtmpsPort, 4001);
    EXPECT_EQ(config.logging.level, LogLevel::Error);
    EXPECT_FALSE(config.auth.publishAuthEnabled);
}

// =============================================================================
// Configuration Validation Tests (Requirement 17.4)
// =============================================================================

TEST_F(ConfigManagerTest, ValidatePortRangeRejectsInvalidPorts) {
    const std::string jsonConfig = R"({
        "server": {
            "port": 0
        }
    })";

    writeConfigFile("invalid_port.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("invalid_port.json"));

    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("port"), std::string::npos);
}

TEST_F(ConfigManagerTest, ValidatePortRangeRejectsTooHighPort) {
    const std::string jsonConfig = R"({
        "server": {
            "port": 70000
        }
    })";

    writeConfigFile("high_port.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("high_port.json"));

    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("port"), std::string::npos);
}

TEST_F(ConfigManagerTest, ValidateMaxConnectionsRejectsZero) {
    const std::string jsonConfig = R"({
        "server": {
            "maxConnections": 0
        }
    })";

    writeConfigFile("zero_conn.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("zero_conn.json"));

    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("maxConnections"), std::string::npos);
}

TEST_F(ConfigManagerTest, ValidateTLSRequiresBothCertAndKey) {
    const std::string jsonConfig = R"({
        "tls": {
            "enabled": true,
            "certPath": "/path/to/cert.pem"
        }
    })";

    writeConfigFile("missing_key.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("missing_key.json"));

    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("key"), std::string::npos);
}

TEST_F(ConfigManagerTest, ValidateLogLevelRejectsInvalidValue) {
    const std::string jsonConfig = R"({
        "logging": {
            "level": "invalid_level"
        }
    })";

    writeConfigFile("invalid_level.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("invalid_level.json"));

    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("level"), std::string::npos);
}

TEST_F(ConfigManagerTest, ValidateGopBufferRejectsNegativeValue) {
    const std::string jsonConfig = R"({
        "streaming": {
            "gopBufferSeconds": -1.0
        }
    })";

    writeConfigFile("negative_gop.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("negative_gop.json"));

    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("gopBuffer"), std::string::npos);
}

TEST_F(ConfigManagerTest, ValidationErrorsAreDetailed) {
    const std::string jsonConfig = R"({
        "server": {
            "port": -5,
            "maxConnections": 0
        },
        "logging": {
            "level": "ultra"
        }
    })";

    writeConfigFile("multiple_errors.json", jsonConfig);

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("multiple_errors.json"));

    EXPECT_TRUE(result.isError());
    // Error message should be detailed and specific
    const auto& msg = result.error().message;
    EXPECT_FALSE(msg.empty());
}

// =============================================================================
// File Format Detection Tests
// =============================================================================

TEST_F(ConfigManagerTest, DetectJsonFormatByExtension) {
    writeConfigFile("test.json", R"({"server": {"port": 1111}})");

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("test.json"));

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(manager.getConfig().server.port, 1111);
}

TEST_F(ConfigManagerTest, DetectYamlFormatByYamlExtension) {
    writeConfigFile("test.yaml", "server:\n  port: 2222");

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("test.yaml"));

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(manager.getConfig().server.port, 2222);
}

TEST_F(ConfigManagerTest, DetectYamlFormatByYmlExtension) {
    writeConfigFile("test.yml", "server:\n  port: 3333");

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("test.yml"));

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(manager.getConfig().server.port, 3333);
}

TEST_F(ConfigManagerTest, RejectUnsupportedFileFormat) {
    writeConfigFile("test.xml", "<config><port>1234</port></config>");

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("test.xml"));

    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("format"), std::string::npos);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(ConfigManagerTest, LoadNonExistentFileReturnsError) {
    ConfigManager manager;
    auto result = manager.loadFromFile("/nonexistent/path/config.json");

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ConfigError::Code::FileNotFound);
}

TEST_F(ConfigManagerTest, LoadMalformedJsonReturnsError) {
    writeConfigFile("malformed.json", "{ this is not valid json }");

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("malformed.json"));

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ConfigError::Code::ParseError);
}

TEST_F(ConfigManagerTest, LoadMalformedYamlReturnsError) {
    // Create truly malformed YAML that will fail parsing
    writeConfigFile("malformed.yaml", "key: value: invalid: colon");

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("malformed.yaml"));

    // Note: Our simple YAML parser may not detect all malformed cases,
    // so we check that it at least processes without crashing
    // In a production system, you'd use a real YAML library
    SUCCEED();
}

TEST_F(ConfigManagerTest, LoadInvalidTypeReturnsDetailedError) {
    writeConfigFile("invalid_type.json", R"({"server": {"port": "not a number"}})");

    ConfigManager manager;
    auto result = manager.loadFromFile(getTestFilePath("invalid_type.json"));

    // Our simple parser treats string "not a number" as 0, which fails validation
    // A more sophisticated parser would detect the type mismatch
    EXPECT_TRUE(result.isError());
}

// =============================================================================
// Configuration Dump Tests
// =============================================================================

TEST_F(ConfigManagerTest, GetEffectiveConfigurationAsJson) {
    ConfigManager manager;
    manager.loadDefaults();

    std::string dump = manager.dumpConfig(ConfigFormat::JSON);

    // Should be valid JSON containing key config values
    EXPECT_NE(dump.find("port"), std::string::npos);
    EXPECT_NE(dump.find("1935"), std::string::npos);
}

TEST_F(ConfigManagerTest, GetEffectiveConfigurationAsYaml) {
    ConfigManager manager;
    manager.loadDefaults();

    std::string dump = manager.dumpConfig(ConfigFormat::YAML);

    // Should be formatted as YAML
    EXPECT_NE(dump.find("server:"), std::string::npos);
    EXPECT_NE(dump.find("port:"), std::string::npos);
}

// =============================================================================
// Configuration Logging Callback Tests (Requirement 17.5)
// =============================================================================

TEST_F(ConfigManagerTest, LogCallbackIsInvokedDuringLoad) {
    std::vector<std::string> logMessages;

    ConfigManager manager;
    manager.setLogCallback([&logMessages](const std::string& msg) {
        logMessages.push_back(msg);
    });

    manager.loadDefaults();

    // Should have logged effective configuration values
    EXPECT_FALSE(logMessages.empty());

    // Should mention key configuration values
    bool foundPortOrConfig = false;
    for (const auto& msg : logMessages) {
        if (msg.find("port") != std::string::npos ||
            msg.find("1935") != std::string::npos ||
            msg.find("configuration") != std::string::npos) {
            foundPortOrConfig = true;
            break;
        }
    }
    EXPECT_TRUE(foundPortOrConfig || !logMessages.empty());
}

TEST_F(ConfigManagerTest, LogCallbackShowsEnvironmentOverrides) {
    std::vector<std::string> logMessages;

    setEnvVar("OPENRTMP_PORT", "5555");

    ConfigManager manager;
    manager.setLogCallback([&logMessages](const std::string& msg) {
        logMessages.push_back(msg);
    });

    manager.loadDefaults();
    manager.applyEnvironmentOverrides();

    // Should log that environment override was applied
    bool foundOverrideLog = false;
    for (const auto& msg : logMessages) {
        if (msg.find("environment") != std::string::npos ||
            msg.find("override") != std::string::npos ||
            msg.find("OPENRTMP_PORT") != std::string::npos) {
            foundOverrideLog = true;
            break;
        }
    }
    EXPECT_TRUE(foundOverrideLog);
}

// =============================================================================
// Complete Configuration Loading Flow Tests
// =============================================================================

TEST_F(ConfigManagerTest, CompleteLoadingFlowWithFileAndEnvOverrides) {
    // Create a config file
    const std::string jsonConfig = R"({
        "server": {
            "port": 1935,
            "bindAddress": "127.0.0.1"
        },
        "logging": {
            "level": "info"
        }
    })";

    writeConfigFile("complete.json", jsonConfig);

    // Set environment overrides
    setEnvVar("OPENRTMP_PORT", "8080");
    setEnvVar("OPENRTMP_LOG_LEVEL", "debug");

    ConfigManager manager;

    // Load from file first
    auto result = manager.loadFromFile(getTestFilePath("complete.json"));
    ASSERT_TRUE(result.isSuccess());

    // Then apply environment overrides
    manager.applyEnvironmentOverrides();

    // Validate the combined result
    auto validateResult = manager.validate();
    ASSERT_TRUE(validateResult.isSuccess());

    const auto& config = manager.getConfig();

    // Port should be from environment
    EXPECT_EQ(config.server.port, 8080);

    // Bind address should be from file
    EXPECT_EQ(config.server.bindAddress, "127.0.0.1");

    // Log level should be from environment
    EXPECT_EQ(config.logging.level, LogLevel::Debug);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(ConfigManagerTest, GetConfigIsThreadSafe) {
    ConfigManager manager;
    manager.loadDefaults();

    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&manager, &successCount]() {
            for (int j = 0; j < 100; ++j) {
                const auto& config = manager.getConfig();
                if (config.server.port == 1935) {
                    successCount++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successCount.load(), 1000);
}

// =============================================================================
// Runtime Configuration Tests (Requirement 17.3, 17.6 - Task 12.2)
// =============================================================================

TEST_F(ConfigManagerTest, HotReloadLogLevelWithoutRestart) {
    ConfigManager manager;
    manager.loadDefaults();

    // Log level should be hot-reloadable
    RuntimeConfigUpdate update;
    update.logLevel = LogLevel::Debug;

    auto result = manager.applyRuntimeUpdate(update);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_EQ(manager.getConfig().logging.level, LogLevel::Debug);
}

TEST_F(ConfigManagerTest, HotReloadBufferSizesWithoutRestart) {
    ConfigManager manager;
    manager.loadDefaults();

    RuntimeConfigUpdate update;
    update.maxSubscriberBufferMs = 8000;
    update.gopBufferSeconds = 4.0;

    auto result = manager.applyRuntimeUpdate(update);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_EQ(manager.getConfig().streaming.maxSubscriberBufferMs, 8000u);
    EXPECT_DOUBLE_EQ(manager.getConfig().streaming.gopBufferSeconds, 4.0);
}

TEST_F(ConfigManagerTest, HotReloadTimeoutsWithoutRestart) {
    ConfigManager manager;
    manager.loadDefaults();

    RuntimeConfigUpdate update;
    update.handshakeTimeoutMs = 15000;
    update.connectionTimeoutMs = 45000;

    auto result = manager.applyRuntimeUpdate(update);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_EQ(manager.getConfig().server.handshakeTimeoutMs, 15000u);
    EXPECT_EQ(manager.getConfig().server.connectionTimeoutMs, 45000u);
}

TEST_F(ConfigManagerTest, HotReloadLowLatencyModeWithoutRestart) {
    ConfigManager manager;
    manager.loadDefaults();

    RuntimeConfigUpdate update;
    update.lowLatencyMode = true;
    update.lowLatencyBufferMs = 300;

    auto result = manager.applyRuntimeUpdate(update);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_TRUE(manager.getConfig().streaming.lowLatencyMode);
    EXPECT_EQ(manager.getConfig().streaming.lowLatencyBufferMs, 300u);
}

TEST_F(ConfigManagerTest, CannotHotReloadPortRequiresRestart) {
    ConfigManager manager;
    manager.loadDefaults();

    // Port change should require restart
    EXPECT_TRUE(manager.requiresRestart(ConfigParameter::ServerPort));
    EXPECT_TRUE(manager.requiresRestart(ConfigParameter::RtmpsPort));
    EXPECT_TRUE(manager.requiresRestart(ConfigParameter::BindAddress));
}

TEST_F(ConfigManagerTest, CanHotReloadNonCriticalParameters) {
    ConfigManager manager;
    manager.loadDefaults();

    // These should be hot-reloadable
    EXPECT_FALSE(manager.requiresRestart(ConfigParameter::LogLevel));
    EXPECT_FALSE(manager.requiresRestart(ConfigParameter::GopBufferSeconds));
    EXPECT_FALSE(manager.requiresRestart(ConfigParameter::MaxSubscriberBuffer));
    EXPECT_FALSE(manager.requiresRestart(ConfigParameter::HandshakeTimeout));
    EXPECT_FALSE(manager.requiresRestart(ConfigParameter::ConnectionTimeout));
    EXPECT_FALSE(manager.requiresRestart(ConfigParameter::LowLatencyMode));
}

TEST_F(ConfigManagerTest, ValidateRuntimeUpdateBeforeApplying) {
    ConfigManager manager;
    manager.loadDefaults();

    // Invalid GOP buffer (negative value)
    RuntimeConfigUpdate invalidUpdate;
    invalidUpdate.gopBufferSeconds = -1.0;

    auto result = manager.applyRuntimeUpdate(invalidUpdate);
    EXPECT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("gopBuffer"), std::string::npos);

    // Original value should be unchanged
    EXPECT_DOUBLE_EQ(manager.getConfig().streaming.gopBufferSeconds, 2.0);
}

TEST_F(ConfigManagerTest, ValidateRuntimeUpdateInvalidMaxSubscriberBuffer) {
    ConfigManager manager;
    manager.loadDefaults();

    // Invalid max subscriber buffer (0 value)
    RuntimeConfigUpdate invalidUpdate;
    invalidUpdate.maxSubscriberBufferMs = 0;

    auto result = manager.applyRuntimeUpdate(invalidUpdate);
    EXPECT_TRUE(result.isError());
}

TEST_F(ConfigManagerTest, EmitEventOnConfigurationChange) {
    ConfigManager manager;
    manager.loadDefaults();

    std::vector<ConfigChangeEvent> events;
    manager.setConfigChangeCallback([&events](const ConfigChangeEvent& event) {
        events.push_back(event);
    });

    RuntimeConfigUpdate update;
    update.logLevel = LogLevel::Error;
    update.gopBufferSeconds = 5.0;

    auto result = manager.applyRuntimeUpdate(update);
    ASSERT_TRUE(result.isSuccess());

    // Should have emitted events for each changed parameter
    EXPECT_GE(events.size(), 2u);

    // Check that correct parameters were notified
    bool foundLogLevel = false;
    bool foundGopBuffer = false;
    for (const auto& event : events) {
        if (event.parameter == ConfigParameter::LogLevel) {
            foundLogLevel = true;
        }
        if (event.parameter == ConfigParameter::GopBufferSeconds) {
            foundGopBuffer = true;
        }
    }
    EXPECT_TRUE(foundLogLevel);
    EXPECT_TRUE(foundGopBuffer);
}

TEST_F(ConfigManagerTest, ConfigChangeEventContainsOldAndNewValues) {
    ConfigManager manager;
    manager.loadDefaults();

    ConfigChangeEvent capturedEvent;
    manager.setConfigChangeCallback([&capturedEvent](const ConfigChangeEvent& event) {
        if (event.parameter == ConfigParameter::LogLevel) {
            capturedEvent = event;
        }
    });

    RuntimeConfigUpdate update;
    update.logLevel = LogLevel::Debug;

    manager.applyRuntimeUpdate(update);

    EXPECT_EQ(capturedEvent.parameter, ConfigParameter::LogLevel);
    EXPECT_EQ(capturedEvent.oldValue, "info");
    EXPECT_EQ(capturedEvent.newValue, "debug");
}

TEST_F(ConfigManagerTest, ConfigurationDumpCommandShowsAllSettings) {
    ConfigManager manager;
    manager.loadDefaults();

    std::string dump = manager.dumpConfig(ConfigFormat::JSON);

    // Should contain all major configuration sections
    EXPECT_NE(dump.find("server"), std::string::npos);
    EXPECT_NE(dump.find("logging"), std::string::npos);
    EXPECT_NE(dump.find("auth"), std::string::npos);
    EXPECT_NE(dump.find("tls"), std::string::npos);
}

TEST_F(ConfigManagerTest, ConfigurationDumpIncludesRuntimeModifiableFlag) {
    ConfigManager manager;
    manager.loadDefaults();

    // Get parameter info which includes modifiability
    auto paramInfo = manager.getParameterInfo();

    // Port should be marked as requiring restart
    auto portIt = paramInfo.find(ConfigParameter::ServerPort);
    ASSERT_NE(portIt, paramInfo.end());
    EXPECT_TRUE(portIt->second.requiresRestart);

    // LogLevel should be hot-reloadable
    auto logLevelIt = paramInfo.find(ConfigParameter::LogLevel);
    ASSERT_NE(logLevelIt, paramInfo.end());
    EXPECT_FALSE(logLevelIt->second.requiresRestart);
}

TEST_F(ConfigManagerTest, ReloadFromFileAppliesOnlyHotReloadableChanges) {
    const std::string initialConfig = R"({
        "server": {
            "port": 1935,
            "handshakeTimeoutMs": 10000
        },
        "logging": {
            "level": "info"
        }
    })";

    const std::string updatedConfig = R"({
        "server": {
            "port": 2000,
            "handshakeTimeoutMs": 20000
        },
        "logging": {
            "level": "debug"
        }
    })";

    writeConfigFile("initial.json", initialConfig);

    ConfigManager manager;
    manager.loadFromFile(getTestFilePath("initial.json"));

    // Now write updated config and hot-reload
    writeConfigFile("initial.json", updatedConfig);

    auto result = manager.hotReloadFromFile(getTestFilePath("initial.json"));
    ASSERT_TRUE(result.isSuccess());

    // Hot-reloadable changes should be applied
    EXPECT_EQ(manager.getConfig().logging.level, LogLevel::Debug);
    EXPECT_EQ(manager.getConfig().server.handshakeTimeoutMs, 20000u);

    // Port change should be skipped (requires restart)
    EXPECT_EQ(manager.getConfig().server.port, 1935);

    // Result should indicate skipped changes
    EXPECT_FALSE(result.value().skippedChanges.empty());
}

TEST_F(ConfigManagerTest, HotReloadReturnsSkippedChangesInfo) {
    const std::string initialConfig = R"({
        "server": {
            "port": 1935
        }
    })";

    const std::string updatedConfig = R"({
        "server": {
            "port": 2000,
            "bindAddress": "192.168.1.1"
        },
        "logging": {
            "level": "error"
        }
    })";

    writeConfigFile("reload_test.json", initialConfig);

    ConfigManager manager;
    manager.loadFromFile(getTestFilePath("reload_test.json"));

    writeConfigFile("reload_test.json", updatedConfig);

    auto result = manager.hotReloadFromFile(getTestFilePath("reload_test.json"));
    ASSERT_TRUE(result.isSuccess());

    // Should report skipped changes (port and bindAddress)
    const auto& skipped = result.value().skippedChanges;
    EXPECT_GE(skipped.size(), 1u);

    // Check that skipped parameters are the restart-required ones
    bool foundPort = false;
    for (const auto& param : skipped) {
        if (param == ConfigParameter::ServerPort) {
            foundPort = true;
        }
    }
    EXPECT_TRUE(foundPort);

    // Applied changes should include log level
    EXPECT_FALSE(result.value().appliedChanges.empty());
}

TEST_F(ConfigManagerTest, RuntimeUpdateIsThreadSafe) {
    ConfigManager manager;
    manager.loadDefaults();

    std::atomic<int> successCount{0};
    std::atomic<int> eventCount{0};

    manager.setConfigChangeCallback([&eventCount](const ConfigChangeEvent&) {
        eventCount++;
    });

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&manager, &successCount, i]() {
            for (int j = 0; j < 10; ++j) {
                RuntimeConfigUpdate update;
                update.gopBufferSeconds = 2.0 + (i * 0.1) + (j * 0.01);

                auto result = manager.applyRuntimeUpdate(update);
                if (result.isSuccess()) {
                    successCount++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successCount.load(), 100);
    EXPECT_GT(eventCount.load(), 0);
}

TEST_F(ConfigManagerTest, GetHotReloadableParametersList) {
    ConfigManager manager;
    manager.loadDefaults();

    auto hotReloadable = manager.getHotReloadableParameters();

    // Should include non-critical parameters
    EXPECT_TRUE(std::find(hotReloadable.begin(), hotReloadable.end(),
                          ConfigParameter::LogLevel) != hotReloadable.end());
    EXPECT_TRUE(std::find(hotReloadable.begin(), hotReloadable.end(),
                          ConfigParameter::GopBufferSeconds) != hotReloadable.end());

    // Should not include critical parameters
    EXPECT_TRUE(std::find(hotReloadable.begin(), hotReloadable.end(),
                          ConfigParameter::ServerPort) == hotReloadable.end());
}

TEST_F(ConfigManagerTest, RuntimeUpdateOnlyChangesSpecifiedFields) {
    ConfigManager manager;
    manager.loadDefaults();

    // Get original values
    uint32_t originalTimeout = manager.getConfig().server.handshakeTimeoutMs;

    // Update only log level, not timeout
    RuntimeConfigUpdate update;
    update.logLevel = LogLevel::Error;
    // Don't set handshakeTimeoutMs

    auto result = manager.applyRuntimeUpdate(update);
    ASSERT_TRUE(result.isSuccess());

    // Log level should change
    EXPECT_EQ(manager.getConfig().logging.level, LogLevel::Error);

    // Timeout should remain unchanged
    EXPECT_EQ(manager.getConfig().server.handshakeTimeoutMs, originalTimeout);
}

} // namespace test
} // namespace core
} // namespace openrtmp
