// OpenRTMP - Cross-platform RTMP Server
// Tests for Stream Registry
//
// Tests cover:
// - Maintain map of active streams by stream key
// - Track publisher and subscriber associations per stream
// - Allocate and release stream IDs atomically
// - Support concurrent access with appropriate locking
// - Emit domain events for stream lifecycle changes
//
// Requirements coverage:
// - Requirement 3.2: Allocate stream ID and return it
// - Requirement 3.7: Stream key conflict detection
// - Requirement 4.1: Store stream with associated stream key

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <set>
#include <mutex>

#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class StreamRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_unique<StreamRegistry>();
    }

    void TearDown() override {
        registry_.reset();
    }

    std::unique_ptr<StreamRegistry> registry_;
};

// =============================================================================
// Stream ID Allocation Tests
// =============================================================================

TEST_F(StreamRegistryTest, AllocateStreamIdReturnsValidId) {
    auto result = registry_->allocateStreamId();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), core::INVALID_STREAM_ID);
}

TEST_F(StreamRegistryTest, AllocateStreamIdReturnsUniqueIds) {
    auto result1 = registry_->allocateStreamId();
    auto result2 = registry_->allocateStreamId();
    auto result3 = registry_->allocateStreamId();

    ASSERT_TRUE(result1.isSuccess());
    ASSERT_TRUE(result2.isSuccess());
    ASSERT_TRUE(result3.isSuccess());

    EXPECT_NE(result1.value(), result2.value());
    EXPECT_NE(result2.value(), result3.value());
    EXPECT_NE(result1.value(), result3.value());
}

TEST_F(StreamRegistryTest, AllocateStreamIdStartsFromOne) {
    auto result = registry_->allocateStreamId();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value(), 1u);
}

TEST_F(StreamRegistryTest, AllocateStreamIdIncrementsSequentially) {
    auto result1 = registry_->allocateStreamId();
    auto result2 = registry_->allocateStreamId();

    ASSERT_TRUE(result1.isSuccess());
    ASSERT_TRUE(result2.isSuccess());

    EXPECT_EQ(result2.value(), result1.value() + 1);
}

TEST_F(StreamRegistryTest, ReleaseStreamIdAllowsReuse) {
    auto result1 = registry_->allocateStreamId();
    ASSERT_TRUE(result1.isSuccess());

    // Release the stream ID
    registry_->releaseStreamId(result1.value());

    // Allocate again - might get the same ID back (implementation defined)
    auto result2 = registry_->allocateStreamId();
    ASSERT_TRUE(result2.isSuccess());
    EXPECT_NE(result2.value(), core::INVALID_STREAM_ID);
}

// =============================================================================
// Stream Registration Tests
// =============================================================================

TEST_F(StreamRegistryTest, RegisterStreamSucceeds) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    PublisherId publisherId = 100;

    auto result = registry_->registerStream(key, streamId, publisherId);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(StreamRegistryTest, RegisterStreamWithDuplicateKeyFails) {
    StreamKey key("live", "test_stream");
    auto streamId1 = registry_->allocateStreamId().value();
    auto streamId2 = registry_->allocateStreamId().value();
    PublisherId publisherId1 = 100;
    PublisherId publisherId2 = 101;

    auto result1 = registry_->registerStream(key, streamId1, publisherId1);
    ASSERT_TRUE(result1.isSuccess());

    // Attempting to register same key should fail (Requirement 3.7)
    auto result2 = registry_->registerStream(key, streamId2, publisherId2);
    ASSERT_TRUE(result2.isError());
    EXPECT_EQ(result2.error().code, StreamRegistryError::Code::StreamKeyInUse);
}

TEST_F(StreamRegistryTest, RegisterStreamWithDifferentKeysSucceeds) {
    StreamKey key1("live", "stream1");
    StreamKey key2("live", "stream2");
    auto streamId1 = registry_->allocateStreamId().value();
    auto streamId2 = registry_->allocateStreamId().value();

    auto result1 = registry_->registerStream(key1, streamId1, 100);
    auto result2 = registry_->registerStream(key2, streamId2, 101);

    ASSERT_TRUE(result1.isSuccess());
    ASSERT_TRUE(result2.isSuccess());
}

TEST_F(StreamRegistryTest, UnregisterStreamSucceeds) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();

    registry_->registerStream(key, streamId, 100);

    auto result = registry_->unregisterStream(key);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(StreamRegistryTest, UnregisterNonexistentStreamFails) {
    StreamKey key("live", "nonexistent");

    auto result = registry_->unregisterStream(key);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, StreamRegistryError::Code::StreamNotFound);
}

TEST_F(StreamRegistryTest, UnregisterAllowsReregistration) {
    StreamKey key("live", "test_stream");
    auto streamId1 = registry_->allocateStreamId().value();

    registry_->registerStream(key, streamId1, 100);
    registry_->unregisterStream(key);

    // Should be able to register same key again
    auto streamId2 = registry_->allocateStreamId().value();
    auto result = registry_->registerStream(key, streamId2, 101);

    ASSERT_TRUE(result.isSuccess());
}

// =============================================================================
// Stream Lookup Tests
// =============================================================================

TEST_F(StreamRegistryTest, FindStreamByKeyReturnsStreamInfo) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    PublisherId publisherId = 100;

    registry_->registerStream(key, streamId, publisherId);

    auto result = registry_->findStream(key);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->key, key);
    EXPECT_EQ(result->streamId, streamId);
    EXPECT_EQ(result->publisherId, publisherId);
    EXPECT_EQ(result->state, StreamState::Publishing);
}

TEST_F(StreamRegistryTest, FindStreamByKeyReturnsNulloptForNonexistent) {
    StreamKey key("live", "nonexistent");

    auto result = registry_->findStream(key);

    EXPECT_FALSE(result.has_value());
}

TEST_F(StreamRegistryTest, FindStreamByIdReturnsStreamInfo) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();

    registry_->registerStream(key, streamId, 100);

    auto result = registry_->findStreamById(streamId);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->key, key);
    EXPECT_EQ(result->streamId, streamId);
}

TEST_F(StreamRegistryTest, FindStreamByIdReturnsNulloptForNonexistent) {
    auto result = registry_->findStreamById(999);

    EXPECT_FALSE(result.has_value());
}

TEST_F(StreamRegistryTest, HasStreamReturnsTrueForExisting) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();

    registry_->registerStream(key, streamId, 100);

    EXPECT_TRUE(registry_->hasStream(key));
}

TEST_F(StreamRegistryTest, HasStreamReturnsFalseForNonexistent) {
    StreamKey key("live", "nonexistent");

    EXPECT_FALSE(registry_->hasStream(key));
}

// =============================================================================
// Subscriber Management Tests
// =============================================================================

TEST_F(StreamRegistryTest, AddSubscriberSucceeds) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    SubscriberId subscriberId = 200;
    auto result = registry_->addSubscriber(key, subscriberId);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(StreamRegistryTest, AddSubscriberToNonexistentStreamFails) {
    StreamKey key("live", "nonexistent");
    SubscriberId subscriberId = 200;

    auto result = registry_->addSubscriber(key, subscriberId);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, StreamRegistryError::Code::StreamNotFound);
}

TEST_F(StreamRegistryTest, AddDuplicateSubscriberFails) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    SubscriberId subscriberId = 200;
    registry_->addSubscriber(key, subscriberId);

    auto result = registry_->addSubscriber(key, subscriberId);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, StreamRegistryError::Code::SubscriberExists);
}

TEST_F(StreamRegistryTest, RemoveSubscriberSucceeds) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    SubscriberId subscriberId = 200;
    registry_->addSubscriber(key, subscriberId);

    auto result = registry_->removeSubscriber(key, subscriberId);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(StreamRegistryTest, RemoveNonexistentSubscriberFails) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    auto result = registry_->removeSubscriber(key, 999);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, StreamRegistryError::Code::SubscriberNotFound);
}

TEST_F(StreamRegistryTest, GetSubscribersReturnsAllSubscribers) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    registry_->addSubscriber(key, 200);
    registry_->addSubscriber(key, 201);
    registry_->addSubscriber(key, 202);

    auto subscribers = registry_->getSubscribers(key);

    EXPECT_EQ(subscribers.size(), 3u);
    EXPECT_TRUE(subscribers.count(200) > 0);
    EXPECT_TRUE(subscribers.count(201) > 0);
    EXPECT_TRUE(subscribers.count(202) > 0);
}

TEST_F(StreamRegistryTest, GetSubscribersReturnsEmptySetForNonexistent) {
    StreamKey key("live", "nonexistent");

    auto subscribers = registry_->getSubscribers(key);

    EXPECT_TRUE(subscribers.empty());
}

TEST_F(StreamRegistryTest, GetSubscriberCountReturnsCorrectCount) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    EXPECT_EQ(registry_->getSubscriberCount(key), 0u);

    registry_->addSubscriber(key, 200);
    EXPECT_EQ(registry_->getSubscriberCount(key), 1u);

    registry_->addSubscriber(key, 201);
    EXPECT_EQ(registry_->getSubscriberCount(key), 2u);

    registry_->removeSubscriber(key, 200);
    EXPECT_EQ(registry_->getSubscriberCount(key), 1u);
}

// =============================================================================
// Event Emission Tests
// =============================================================================

TEST_F(StreamRegistryTest, RegisterStreamEmitsStreamPublishedEvent) {
    bool eventReceived = false;
    StreamKey receivedKey;

    registry_->setEventCallback([&](const StreamEvent& event) {
        if (event.type == StreamEvent::Type::StreamPublished) {
            eventReceived = true;
            receivedKey = event.streamKey;
        }
    });

    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    EXPECT_TRUE(eventReceived);
    EXPECT_EQ(receivedKey, key);
}

TEST_F(StreamRegistryTest, UnregisterStreamEmitsStreamEndedEvent) {
    bool eventReceived = false;
    StreamKey receivedKey;

    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    registry_->setEventCallback([&](const StreamEvent& event) {
        if (event.type == StreamEvent::Type::StreamEnded) {
            eventReceived = true;
            receivedKey = event.streamKey;
        }
    });

    registry_->unregisterStream(key);

    EXPECT_TRUE(eventReceived);
    EXPECT_EQ(receivedKey, key);
}

TEST_F(StreamRegistryTest, AddSubscriberEmitsSubscriberJoinedEvent) {
    bool eventReceived = false;
    SubscriberId receivedSubscriberId = 0;

    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    registry_->setEventCallback([&](const StreamEvent& event) {
        if (event.type == StreamEvent::Type::SubscriberJoined) {
            eventReceived = true;
            receivedSubscriberId = event.subscriberId;
        }
    });

    registry_->addSubscriber(key, 200);

    EXPECT_TRUE(eventReceived);
    EXPECT_EQ(receivedSubscriberId, 200u);
}

TEST_F(StreamRegistryTest, RemoveSubscriberEmitsSubscriberLeftEvent) {
    bool eventReceived = false;
    SubscriberId receivedSubscriberId = 0;

    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);
    registry_->addSubscriber(key, 200);

    registry_->setEventCallback([&](const StreamEvent& event) {
        if (event.type == StreamEvent::Type::SubscriberLeft) {
            eventReceived = true;
            receivedSubscriberId = event.subscriberId;
        }
    });

    registry_->removeSubscriber(key, 200);

    EXPECT_TRUE(eventReceived);
    EXPECT_EQ(receivedSubscriberId, 200u);
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(StreamRegistryTest, GetActiveStreamCountReturnsCorrectCount) {
    EXPECT_EQ(registry_->getActiveStreamCount(), 0u);

    StreamKey key1("live", "stream1");
    StreamKey key2("live", "stream2");

    registry_->registerStream(key1, registry_->allocateStreamId().value(), 100);
    EXPECT_EQ(registry_->getActiveStreamCount(), 1u);

    registry_->registerStream(key2, registry_->allocateStreamId().value(), 101);
    EXPECT_EQ(registry_->getActiveStreamCount(), 2u);

    registry_->unregisterStream(key1);
    EXPECT_EQ(registry_->getActiveStreamCount(), 1u);
}

TEST_F(StreamRegistryTest, GetAllStreamKeysReturnsAllKeys) {
    StreamKey key1("live", "stream1");
    StreamKey key2("live", "stream2");
    StreamKey key3("vod", "stream3");

    registry_->registerStream(key1, registry_->allocateStreamId().value(), 100);
    registry_->registerStream(key2, registry_->allocateStreamId().value(), 101);
    registry_->registerStream(key3, registry_->allocateStreamId().value(), 102);

    auto keys = registry_->getAllStreamKeys();

    EXPECT_EQ(keys.size(), 3u);
    EXPECT_TRUE(std::find(keys.begin(), keys.end(), key1) != keys.end());
    EXPECT_TRUE(std::find(keys.begin(), keys.end(), key2) != keys.end());
    EXPECT_TRUE(std::find(keys.begin(), keys.end(), key3) != keys.end());
}

// =============================================================================
// Concurrent Access Tests
// =============================================================================

class StreamRegistryConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_unique<StreamRegistry>();
    }

    std::unique_ptr<StreamRegistry> registry_;
};

TEST_F(StreamRegistryConcurrencyTest, ConcurrentStreamIdAllocationIsThreadSafe) {
    const int numThreads = 10;
    const int allocationsPerThread = 100;
    std::vector<std::thread> threads;
    std::vector<StreamId> allIds;
    std::mutex idsMutex;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &allIds, &idsMutex, allocationsPerThread]() {
            for (int j = 0; j < allocationsPerThread; ++j) {
                auto result = registry_->allocateStreamId();
                if (result.isSuccess()) {
                    std::lock_guard<std::mutex> lock(idsMutex);
                    allIds.push_back(result.value());
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All IDs should be unique
    std::set<StreamId> uniqueIds(allIds.begin(), allIds.end());
    EXPECT_EQ(uniqueIds.size(), allIds.size());
    EXPECT_EQ(allIds.size(), static_cast<size_t>(numThreads * allocationsPerThread));
}

TEST_F(StreamRegistryConcurrencyTest, ConcurrentStreamRegistrationIsThreadSafe) {
    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    std::atomic<int> failCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i, &successCount, &failCount]() {
            StreamKey key("live", "stream_" + std::to_string(i));
            auto streamId = registry_->allocateStreamId().value();
            auto result = registry_->registerStream(key, streamId, static_cast<PublisherId>(i));
            if (result.isSuccess()) {
                successCount++;
            } else {
                failCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All registrations should succeed since keys are unique
    EXPECT_EQ(successCount.load(), numThreads);
    EXPECT_EQ(failCount.load(), 0);
}

TEST_F(StreamRegistryConcurrencyTest, ConcurrentSameKeyRegistrationOnlyOneSucceeds) {
    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    std::atomic<int> failCount{0};

    StreamKey key("live", "same_stream");

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &key, i, &successCount, &failCount]() {
            auto streamId = registry_->allocateStreamId().value();
            auto result = registry_->registerStream(key, streamId, static_cast<PublisherId>(i));
            if (result.isSuccess()) {
                successCount++;
            } else {
                failCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Only one registration should succeed (Requirement 3.7)
    EXPECT_EQ(successCount.load(), 1);
    EXPECT_EQ(failCount.load(), numThreads - 1);
}

TEST_F(StreamRegistryConcurrencyTest, ConcurrentSubscriberAdditionIsThreadSafe) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &key, i, &successCount]() {
            auto result = registry_->addSubscriber(key, static_cast<SubscriberId>(200 + i));
            if (result.isSuccess()) {
                successCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successCount.load(), numThreads);
    EXPECT_EQ(registry_->getSubscriberCount(key), static_cast<size_t>(numThreads));
}

TEST_F(StreamRegistryConcurrencyTest, ConcurrentReadWriteIsThreadSafe) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    const int numReaders = 5;
    const int numWriters = 3;
    const int operationsPerThread = 50;

    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Reader threads
    for (int i = 0; i < numReaders; ++i) {
        threads.emplace_back([this, &key, &running, operationsPerThread]() {
            for (int j = 0; j < operationsPerThread && running; ++j) {
                registry_->findStream(key);
                registry_->hasStream(key);
                registry_->getSubscriberCount(key);
                std::this_thread::yield();
            }
        });
    }

    // Writer threads (add/remove subscribers)
    for (int i = 0; i < numWriters; ++i) {
        threads.emplace_back([this, &key, i, operationsPerThread]() {
            for (int j = 0; j < operationsPerThread; ++j) {
                SubscriberId subId = static_cast<SubscriberId>(1000 + i * 100 + j);
                registry_->addSubscriber(key, subId);
                std::this_thread::yield();
                registry_->removeSubscriber(key, subId);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // No crashes or data corruption should occur
    EXPECT_TRUE(registry_->hasStream(key));
}

// =============================================================================
// Stream Metadata Tests
// =============================================================================

TEST_F(StreamRegistryTest, UpdateStreamMetadataSucceeds) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    StreamMetadata metadata;
    metadata.title = "Test Stream";
    metadata.description = "A test stream";

    auto result = registry_->updateMetadata(key, metadata);

    ASSERT_TRUE(result.isSuccess());

    auto stream = registry_->findStream(key);
    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(stream->metadata.title, "Test Stream");
}

TEST_F(StreamRegistryTest, UpdateMetadataForNonexistentStreamFails) {
    StreamKey key("live", "nonexistent");
    StreamMetadata metadata;

    auto result = registry_->updateMetadata(key, metadata);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, StreamRegistryError::Code::StreamNotFound);
}

TEST_F(StreamRegistryTest, UpdateCodecInfoSucceeds) {
    StreamKey key("live", "test_stream");
    auto streamId = registry_->allocateStreamId().value();
    registry_->registerStream(key, streamId, 100);

    CodecInfo codecInfo;
    codecInfo.videoCodec = VideoCodec::H264;
    codecInfo.audioCodec = AudioCodec::AAC;
    codecInfo.width = 1920;
    codecInfo.height = 1080;

    auto result = registry_->updateCodecInfo(key, codecInfo);

    ASSERT_TRUE(result.isSuccess());

    auto stream = registry_->findStream(key);
    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(stream->codecInfo.videoCodec, VideoCodec::H264);
    EXPECT_EQ(stream->codecInfo.width, 1920u);
}

// =============================================================================
// Clear All Tests
// =============================================================================

TEST_F(StreamRegistryTest, ClearRemovesAllStreams) {
    StreamKey key1("live", "stream1");
    StreamKey key2("live", "stream2");

    registry_->registerStream(key1, registry_->allocateStreamId().value(), 100);
    registry_->registerStream(key2, registry_->allocateStreamId().value(), 101);

    EXPECT_EQ(registry_->getActiveStreamCount(), 2u);

    registry_->clear();

    EXPECT_EQ(registry_->getActiveStreamCount(), 0u);
    EXPECT_FALSE(registry_->hasStream(key1));
    EXPECT_FALSE(registry_->hasStream(key2));
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
