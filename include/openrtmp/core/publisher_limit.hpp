// OpenRTMP - Cross-platform RTMP Server
// Publisher Limit - Manages simultaneous publishing stream limits
//
// Responsibilities:
// - Enforce 10 simultaneous publishers on desktop platforms
// - Enforce 3 simultaneous publishers on mobile platforms
// - Return error response when publishing limits reached
// - Track and report current publisher count in metrics
//
// Requirements coverage:
// - Requirement 14.3: Desktop platforms support at least 10 simultaneous publishing streams
// - Requirement 14.4: Mobile platforms support at least 3 simultaneous publishing streams

#ifndef OPENRTMP_CORE_PUBLISHER_LIMIT_HPP
#define OPENRTMP_CORE_PUBLISHER_LIMIT_HPP

#include <cstdint>
#include <string>
#include <set>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <chrono>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/error_codes.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// Constants
// =============================================================================

/// Desktop platforms support at least 10 simultaneous publishing streams (Requirement 14.3)
constexpr size_t DESKTOP_PUBLISHER_LIMIT = 10;

/// Mobile platforms support at least 3 simultaneous publishing streams (Requirement 14.4)
constexpr size_t MOBILE_PUBLISHER_LIMIT = 3;

// =============================================================================
// Configuration Types
// =============================================================================

/**
 * @brief Configuration for publisher limits.
 *
 * Controls the maximum number of simultaneous publishers
 * and threshold notifications.
 */
struct PublisherLimitConfig {
    /// Maximum number of simultaneous publishers
    size_t maxPublishers{DESKTOP_PUBLISHER_LIMIT};

    /// High watermark percentage for warning (0-100)
    uint32_t highWatermarkPercent{80};

    /// Low watermark percentage for recovery notification (0-100)
    uint32_t lowWatermarkPercent{50};

    /**
     * @brief Create platform-appropriate default configuration.
     *
     * Uses DESKTOP_PUBLISHER_LIMIT (10) for desktop platforms
     * and MOBILE_PUBLISHER_LIMIT (3) for mobile platforms.
     *
     * @return Configuration with platform-appropriate defaults
     */
    static PublisherLimitConfig platformDefault() {
        PublisherLimitConfig config;
        if (isMobilePlatform()) {
            config.maxPublishers = MOBILE_PUBLISHER_LIMIT;
        } else {
            config.maxPublishers = DESKTOP_PUBLISHER_LIMIT;
        }
        return config;
    }
};

// =============================================================================
// Statistics Types
// =============================================================================

/**
 * @brief Statistics for publisher limit tracking.
 */
struct PublisherLimitStatistics {
    /// Current number of active publishers
    size_t activePublishers{0};

    /// Peak number of simultaneous publishers
    size_t peakPublishers{0};

    /// Total number of successful acquires
    uint64_t totalAcquires{0};

    /// Total number of releases
    uint64_t totalReleases{0};

    /// Total number of rejected acquire attempts
    uint64_t rejectedAcquires{0};

    /// Maximum configured limit
    size_t maxPublishers{0};
};

// =============================================================================
// Event Types
// =============================================================================

/**
 * @brief Event types for publisher limit notifications.
 */
enum class PublisherLimitEventType {
    /// Publisher successfully acquired a slot
    PublisherAcquired,

    /// Publisher released its slot
    PublisherReleased,

    /// Publisher was rejected due to limit
    PublisherRejected,

    /// High watermark threshold reached
    HighWatermark,

    /// Low watermark threshold reached (recovery)
    LowWatermark,

    /// Maximum limit reached
    LimitReached
};

/**
 * @brief Event information for publisher limit callbacks.
 */
struct PublisherLimitEvent {
    /// Event type
    PublisherLimitEventType type{PublisherLimitEventType::LimitReached};

    /// Publisher ID (if applicable)
    PublisherId publisherId{0};

    /// Current publisher count
    size_t currentCount{0};

    /// Maximum publisher limit
    size_t maxCount{0};

    /// Event timestamp
    std::chrono::steady_clock::time_point timestamp{std::chrono::steady_clock::now()};
};

/**
 * @brief Callback type for publisher limit events.
 */
using PublisherLimitCallback = std::function<void(const PublisherLimitEvent&)>;

// =============================================================================
// Publisher Limit Interface
// =============================================================================

/**
 * @brief Interface for publisher limit management.
 *
 * Defines the contract for managing publishing stream limits.
 */
class IPublisherLimit {
public:
    virtual ~IPublisherLimit() = default;

    // -------------------------------------------------------------------------
    // Publisher Slot Management
    // -------------------------------------------------------------------------

    /**
     * @brief Try to acquire a publisher slot.
     *
     * @param publisherId The publisher ID requesting a slot
     * @return Success if slot acquired, error otherwise
     */
    virtual Result<void, Error> acquirePublisherSlot(PublisherId publisherId) = 0;

    /**
     * @brief Release a publisher slot.
     *
     * @param publisherId The publisher ID releasing its slot
     */
    virtual void releasePublisherSlot(PublisherId publisherId) = 0;

    // -------------------------------------------------------------------------
    // Query Methods
    // -------------------------------------------------------------------------

    /**
     * @brief Check if a publisher has an active slot.
     *
     * @param publisherId The publisher ID to check
     * @return true if publisher has an active slot
     */
    virtual bool hasPublisher(PublisherId publisherId) const = 0;

    /**
     * @brief Check if a new publisher can be acquired.
     *
     * @return true if below limit
     */
    virtual bool canAcquire() const = 0;

    /**
     * @brief Get the current number of active publishers.
     *
     * @return Active publisher count
     */
    virtual size_t getActivePublisherCount() const = 0;

    /**
     * @brief Get the number of remaining slots.
     *
     * @return Number of slots available
     */
    virtual size_t getRemainingSlots() const = 0;

    /**
     * @brief Get the set of active publisher IDs.
     *
     * @return Set of active publisher IDs
     */
    virtual std::set<PublisherId> getActivePublisherIds() const = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get publisher limit statistics.
     *
     * @return Current statistics
     */
    virtual PublisherLimitStatistics getStatistics() const = 0;

    /**
     * @brief Reset statistics counters.
     */
    virtual void resetStatistics() = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Get the current configuration.
     *
     * @return Current configuration
     */
    virtual PublisherLimitConfig getConfig() const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for publisher limit events.
     *
     * @param callback The callback function
     */
    virtual void setPublisherLimitCallback(PublisherLimitCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Clear all publishers and release all slots.
     */
    virtual void clear() = 0;
};

// =============================================================================
// Publisher Limit Implementation
// =============================================================================

/**
 * @brief Thread-safe publisher limit implementation.
 *
 * Manages simultaneous publishing stream limits with:
 * - Platform-aware defaults (10 desktop, 3 mobile)
 * - Acquire/release pattern for publisher slots
 * - Statistics tracking for monitoring
 * - Callback notifications for limit events
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 */
class PublisherLimit : public IPublisherLimit {
public:
    /**
     * @brief Construct a new PublisherLimit with configuration.
     *
     * @param config Configuration for the limiter
     */
    explicit PublisherLimit(const PublisherLimitConfig& config = PublisherLimitConfig::platformDefault());

    /**
     * @brief Destructor.
     */
    ~PublisherLimit() override;

    // Non-copyable
    PublisherLimit(const PublisherLimit&) = delete;
    PublisherLimit& operator=(const PublisherLimit&) = delete;

    // Movable
    PublisherLimit(PublisherLimit&&) noexcept;
    PublisherLimit& operator=(PublisherLimit&&) noexcept;

    // IPublisherLimit interface implementation
    Result<void, Error> acquirePublisherSlot(PublisherId publisherId) override;
    void releasePublisherSlot(PublisherId publisherId) override;

    bool hasPublisher(PublisherId publisherId) const override;
    bool canAcquire() const override;
    size_t getActivePublisherCount() const override;
    size_t getRemainingSlots() const override;
    std::set<PublisherId> getActivePublisherIds() const override;

    PublisherLimitStatistics getStatistics() const override;
    void resetStatistics() override;

    PublisherLimitConfig getConfig() const override;

    void setPublisherLimitCallback(PublisherLimitCallback callback) override;

    void clear() override;

private:
    /**
     * @brief Emit a publisher limit event.
     */
    void emitEvent(PublisherLimitEventType type, PublisherId publisherId = 0);

    /**
     * @brief Check and emit watermark events if thresholds crossed.
     */
    void checkWatermarks(size_t previousCount, size_t currentCount);

    /// Configuration
    PublisherLimitConfig config_;

    /// Active publisher IDs
    std::set<PublisherId> activePublishers_;

    /// Statistics
    size_t peakPublishers_{0};
    uint64_t totalAcquires_{0};
    uint64_t totalReleases_{0};
    uint64_t rejectedAcquires_{0};

    /// Watermark tracking
    bool highWatermarkTriggered_{false};

    /// Thread safety
    mutable std::shared_mutex mutex_;

    /// Event callback
    PublisherLimitCallback callback_;
    mutable std::mutex callbackMutex_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_PUBLISHER_LIMIT_HPP
