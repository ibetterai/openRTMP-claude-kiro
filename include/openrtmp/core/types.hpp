// OpenRTMP - Cross-platform RTMP Server
// Common type definitions

#ifndef OPENRTMP_CORE_TYPES_HPP
#define OPENRTMP_CORE_TYPES_HPP

#include <cstdint>
#include <string>
#include <chrono>
#include <functional>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

namespace openrtmp {
namespace core {

// Type aliases for handles and IDs
using ConnectionId = uint64_t;
using SessionId = uint64_t;
using StreamId = uint32_t;
using SubscriberId = uint64_t;
using PublisherId = uint64_t;
using TimerHandle = uint64_t;
using ThreadId = uint64_t;

// Invalid handle constants
constexpr ConnectionId INVALID_CONNECTION_ID = 0;
constexpr SessionId INVALID_SESSION_ID = 0;
constexpr StreamId INVALID_STREAM_ID = 0;
constexpr SubscriberId INVALID_SUBSCRIBER_ID = 0;
constexpr PublisherId INVALID_PUBLISHER_ID = 0;
constexpr TimerHandle INVALID_TIMER_HANDLE = 0;

/**
 * @brief Stream key identifying a unique stream.
 *
 * A stream key consists of an application name and a stream name,
 * combined as "app/stream".
 */
struct StreamKey {
    std::string app;
    std::string name;

    StreamKey() = default;
    StreamKey(std::string a, std::string n)
        : app(std::move(a)), name(std::move(n)) {}

    /**
     * @brief Get the full stream key as a string.
     * @return Full key in format "app/name"
     */
    [[nodiscard]] std::string toString() const {
        return app + "/" + name;
    }

    bool operator==(const StreamKey& other) const {
        return app == other.app && name == other.name;
    }

    bool operator!=(const StreamKey& other) const {
        return !(*this == other);
    }

    bool operator<(const StreamKey& other) const {
        if (app != other.app) return app < other.app;
        return name < other.name;
    }
};

/**
 * @brief Media types supported by RTMP.
 */
enum class MediaType : uint8_t {
    Audio = 8,
    Video = 9,
    Data = 18
};

/**
 * @brief Video frame types.
 */
enum class FrameType : uint8_t {
    Keyframe = 1,
    InterFrame = 2,
    DisposableInterFrame = 3,
    GeneratedKeyframe = 4,
    VideoInfoFrame = 5
};

/**
 * @brief Video codec identifiers.
 */
enum class VideoCodec : uint8_t {
    Unknown = 0,
    H263 = 2,
    ScreenVideo = 3,
    VP6 = 4,
    VP6Alpha = 5,
    ScreenVideo2 = 6,
    H264 = 7,       // AVC
    H265 = 12,      // HEVC (Enhanced RTMP)
    AV1 = 13,       // AV1 (Enhanced RTMP)
    VP9 = 14        // VP9 (Enhanced RTMP)
};

/**
 * @brief Audio codec identifiers.
 */
enum class AudioCodec : uint8_t {
    Unknown = 0,
    ADPCM = 1,
    MP3 = 2,
    LinearPCMLittleEndian = 3,
    Nellymoser16kHz = 4,
    Nellymoser8kHz = 5,
    Nellymoser = 6,
    G711ALaw = 7,
    G711MuLaw = 8,
    AAC = 10,
    Speex = 11,
    MP3_8kHz = 14,
    DeviceSpecific = 15,
    Opus = 16       // Enhanced RTMP
};

/**
 * @brief Connection state enumeration.
 */
enum class ConnectionState {
    Connecting,
    Handshaking,
    Connected,
    Publishing,
    Subscribing,
    Disconnected
};

/**
 * @brief Stream state enumeration.
 */
enum class StreamState {
    Idle,
    Publishing,
    Ended
};

/**
 * @brief Codec information structure.
 */
struct CodecInfo {
    VideoCodec videoCodec = VideoCodec::Unknown;
    AudioCodec audioCodec = AudioCodec::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameRate = 0;
    uint32_t audioBitrate = 0;
    uint32_t videoBitrate = 0;
    uint32_t audioSampleRate = 0;
    uint8_t audioChannels = 0;
};

/**
 * @brief Client information for authentication and logging.
 */
struct ClientInfo {
    std::string ip;
    uint16_t port = 0;
    std::string userAgent;
};

/**
 * @brief Time utilities.
 */
using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using TimePoint = SteadyClock::time_point;
using Duration = std::chrono::milliseconds;

/**
 * @brief Callback types.
 */
using TimerCallback = std::function<void()>;
using WorkItem = std::function<void()>;

/**
 * @brief Network type for mobile platforms.
 */
enum class NetworkType {
    None,
    WiFi,
    Cellular,
    Ethernet,
    Unknown
};

/**
 * @brief Platform type.
 */
enum class Platform {
    Unknown,
    MacOS,
    Windows,
    Linux,
    iOS,
    iPadOS,
    Android
};

/**
 * @brief Get the current platform.
 * @return Current platform
 */
inline Platform currentPlatform() {
#if defined(__APPLE__)
    #if TARGET_OS_IPHONE
        #if TARGET_OS_IOS
            return Platform::iOS;
        #else
            return Platform::iPadOS;
        #endif
    #else
        return Platform::MacOS;
    #endif
#elif defined(_WIN32)
    return Platform::Windows;
#elif defined(__ANDROID__)
    return Platform::Android;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

/**
 * @brief Check if current platform is desktop.
 * @return true if desktop platform
 */
inline bool isDesktopPlatform() {
    Platform p = currentPlatform();
    return p == Platform::MacOS || p == Platform::Windows || p == Platform::Linux;
}

/**
 * @brief Check if current platform is mobile.
 * @return true if mobile platform
 */
inline bool isMobilePlatform() {
    Platform p = currentPlatform();
    return p == Platform::iOS || p == Platform::iPadOS || p == Platform::Android;
}

} // namespace core

// Re-export commonly used types to openrtmp namespace
using core::ConnectionId;
using core::SessionId;
using core::StreamId;
using core::SubscriberId;
using core::PublisherId;
using core::StreamKey;
using core::MediaType;
using core::FrameType;
using core::VideoCodec;
using core::AudioCodec;
using core::ConnectionState;
using core::StreamState;
using core::CodecInfo;
using core::ClientInfo;
using core::Platform;
using core::NetworkType;

} // namespace openrtmp

// Hash specialization for StreamKey
namespace std {
template<>
struct hash<openrtmp::StreamKey> {
    size_t operator()(const openrtmp::StreamKey& key) const {
        size_t h1 = hash<string>{}(key.app);
        size_t h2 = hash<string>{}(key.name);
        return h1 ^ (h2 << 1);
    }
};
} // namespace std

#endif // OPENRTMP_CORE_TYPES_HPP
