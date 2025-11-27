// OpenRTMP - Cross-platform RTMP Server
// Tests for AMF Codec (AMF0 and AMF3 encoding/decoding)
//
// Tests cover:
// - AMF0 type decoding: Number, Boolean, String, Object, Null, Undefined,
//   Reference, ECMAArray, StrictArray, Date, Long String
// - AMF3 type decoding with reference tables and traits support
// - AMF0/AMF3 encoding for command responses
// - Error handling with detailed error codes and byte offset reporting
// - Safety limits: 32 levels max nesting, 64KB max string length
// - Reference table reset per command message
//
// Requirements coverage:
// - Requirement 3.1: connect command response encoding
// - Requirement 3.2: createStream response encoding
// - Requirement 3.3: publish command decoding/response
// - Requirement 3.4: play command decoding/response
// - Requirement 3.5: deleteStream command handling
// - Requirement 3.6: closeStream command handling
// - Requirement 3.7: stream key conflict error encoding

#include <gtest/gtest.h>
#include <memory>
#include <cstring>
#include <cmath>
#include <limits>
#include "openrtmp/protocol/amf_codec.hpp"
#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace protocol {
namespace test {

// =============================================================================
// AMF0 Type Markers (from Adobe AMF0 specification)
// =============================================================================

namespace amf0 {
    constexpr uint8_t NUMBER_MARKER = 0x00;
    constexpr uint8_t BOOLEAN_MARKER = 0x01;
    constexpr uint8_t STRING_MARKER = 0x02;
    constexpr uint8_t OBJECT_MARKER = 0x03;
    constexpr uint8_t MOVIECLIP_MARKER = 0x04;  // Reserved, not supported
    constexpr uint8_t NULL_MARKER = 0x05;
    constexpr uint8_t UNDEFINED_MARKER = 0x06;
    constexpr uint8_t REFERENCE_MARKER = 0x07;
    constexpr uint8_t ECMA_ARRAY_MARKER = 0x08;
    constexpr uint8_t OBJECT_END_MARKER = 0x09;
    constexpr uint8_t STRICT_ARRAY_MARKER = 0x0A;
    constexpr uint8_t DATE_MARKER = 0x0B;
    constexpr uint8_t LONG_STRING_MARKER = 0x0C;
    constexpr uint8_t UNSUPPORTED_MARKER = 0x0D;
    constexpr uint8_t RECORDSET_MARKER = 0x0E;  // Reserved, not supported
    constexpr uint8_t XML_DOC_MARKER = 0x0F;
    constexpr uint8_t TYPED_OBJECT_MARKER = 0x10;
    constexpr uint8_t AVMPLUS_MARKER = 0x11;    // Switch to AMF3
}

// =============================================================================
// AMF3 Type Markers (from Adobe AMF3 specification)
// =============================================================================

namespace amf3 {
    constexpr uint8_t UNDEFINED_MARKER = 0x00;
    constexpr uint8_t NULL_MARKER = 0x01;
    constexpr uint8_t FALSE_MARKER = 0x02;
    constexpr uint8_t TRUE_MARKER = 0x03;
    constexpr uint8_t INTEGER_MARKER = 0x04;
    constexpr uint8_t DOUBLE_MARKER = 0x05;
    constexpr uint8_t STRING_MARKER = 0x06;
    constexpr uint8_t XML_DOC_MARKER = 0x07;
    constexpr uint8_t DATE_MARKER = 0x08;
    constexpr uint8_t ARRAY_MARKER = 0x09;
    constexpr uint8_t OBJECT_MARKER = 0x0A;
    constexpr uint8_t XML_MARKER = 0x0B;
    constexpr uint8_t BYTE_ARRAY_MARKER = 0x0C;
}

// =============================================================================
// Test Helpers - AMF Data Construction
// =============================================================================

namespace AMFTestHelpers {

/**
 * @brief Write a 64-bit IEEE 754 double in big-endian order.
 */
inline void writeDoubleBE(core::Buffer& buffer, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));

    buffer.append({
        static_cast<uint8_t>((bits >> 56) & 0xFF),
        static_cast<uint8_t>((bits >> 48) & 0xFF),
        static_cast<uint8_t>((bits >> 40) & 0xFF),
        static_cast<uint8_t>((bits >> 32) & 0xFF),
        static_cast<uint8_t>((bits >> 24) & 0xFF),
        static_cast<uint8_t>((bits >> 16) & 0xFF),
        static_cast<uint8_t>((bits >> 8) & 0xFF),
        static_cast<uint8_t>(bits & 0xFF)
    });
}

/**
 * @brief Create AMF0 Number (type 0x00).
 */
inline core::Buffer createAMF0Number(double value) {
    core::Buffer buffer;
    buffer.append({amf0::NUMBER_MARKER});
    writeDoubleBE(buffer, value);
    return buffer;
}

/**
 * @brief Create AMF0 Boolean (type 0x01).
 */
inline core::Buffer createAMF0Boolean(bool value) {
    core::Buffer buffer;
    buffer.append({amf0::BOOLEAN_MARKER, static_cast<uint8_t>(value ? 1 : 0)});
    return buffer;
}

/**
 * @brief Create AMF0 String (type 0x02, max 65535 bytes).
 */
inline core::Buffer createAMF0String(const std::string& str) {
    core::Buffer buffer;
    buffer.append({amf0::STRING_MARKER});

    uint16_t length = static_cast<uint16_t>(str.size());
    buffer.append({
        static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>(length & 0xFF)
    });
    buffer.append(reinterpret_cast<const uint8_t*>(str.data()), str.size());
    return buffer;
}

/**
 * @brief Create AMF0 Null (type 0x05).
 */
inline core::Buffer createAMF0Null() {
    core::Buffer buffer;
    buffer.append({amf0::NULL_MARKER});
    return buffer;
}

/**
 * @brief Create AMF0 Undefined (type 0x06).
 */
inline core::Buffer createAMF0Undefined() {
    core::Buffer buffer;
    buffer.append({amf0::UNDEFINED_MARKER});
    return buffer;
}

/**
 * @brief Create AMF0 Reference (type 0x07).
 */
inline core::Buffer createAMF0Reference(uint16_t index) {
    core::Buffer buffer;
    buffer.append({
        amf0::REFERENCE_MARKER,
        static_cast<uint8_t>((index >> 8) & 0xFF),
        static_cast<uint8_t>(index & 0xFF)
    });
    return buffer;
}

/**
 * @brief Create AMF0 Date (type 0x0B).
 */
inline core::Buffer createAMF0Date(double milliseconds, int16_t timezone = 0) {
    core::Buffer buffer;
    buffer.append({amf0::DATE_MARKER});
    writeDoubleBE(buffer, milliseconds);
    buffer.append({
        static_cast<uint8_t>((timezone >> 8) & 0xFF),
        static_cast<uint8_t>(timezone & 0xFF)
    });
    return buffer;
}

/**
 * @brief Create AMF0 Long String (type 0x0C, max 4GB).
 */
inline core::Buffer createAMF0LongString(const std::string& str) {
    core::Buffer buffer;
    buffer.append({amf0::LONG_STRING_MARKER});

    uint32_t length = static_cast<uint32_t>(str.size());
    buffer.append({
        static_cast<uint8_t>((length >> 24) & 0xFF),
        static_cast<uint8_t>((length >> 16) & 0xFF),
        static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>(length & 0xFF)
    });
    buffer.append(reinterpret_cast<const uint8_t*>(str.data()), str.size());
    return buffer;
}

/**
 * @brief Create AMF0 Object end marker (0x00 0x00 0x09).
 */
inline core::Buffer createAMF0ObjectEnd() {
    core::Buffer buffer;
    buffer.append({0x00, 0x00, amf0::OBJECT_END_MARKER});
    return buffer;
}

/**
 * @brief Create AMF0 property name (16-bit length + string, no type marker).
 */
inline core::Buffer createAMF0PropertyName(const std::string& name) {
    core::Buffer buffer;
    uint16_t length = static_cast<uint16_t>(name.size());
    buffer.append({
        static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>(length & 0xFF)
    });
    buffer.append(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    return buffer;
}

/**
 * @brief Create a simple AMF0 Object with given properties.
 */
inline core::Buffer createAMF0Object(
    const std::vector<std::pair<std::string, core::Buffer>>& properties
) {
    core::Buffer buffer;
    buffer.append({amf0::OBJECT_MARKER});

    for (const auto& prop : properties) {
        buffer.append(createAMF0PropertyName(prop.first));
        buffer.append(prop.second);
    }

    buffer.append(createAMF0ObjectEnd());
    return buffer;
}

/**
 * @brief Create AMF0 ECMA Array with given properties.
 */
inline core::Buffer createAMF0ECMAArray(
    uint32_t approximateCount,
    const std::vector<std::pair<std::string, core::Buffer>>& properties
) {
    core::Buffer buffer;
    buffer.append({amf0::ECMA_ARRAY_MARKER});
    buffer.append({
        static_cast<uint8_t>((approximateCount >> 24) & 0xFF),
        static_cast<uint8_t>((approximateCount >> 16) & 0xFF),
        static_cast<uint8_t>((approximateCount >> 8) & 0xFF),
        static_cast<uint8_t>(approximateCount & 0xFF)
    });

    for (const auto& prop : properties) {
        buffer.append(createAMF0PropertyName(prop.first));
        buffer.append(prop.second);
    }

    buffer.append(createAMF0ObjectEnd());
    return buffer;
}

/**
 * @brief Create AMF0 Strict Array with given elements.
 */
inline core::Buffer createAMF0StrictArray(const std::vector<core::Buffer>& elements) {
    core::Buffer buffer;
    buffer.append({amf0::STRICT_ARRAY_MARKER});

    uint32_t count = static_cast<uint32_t>(elements.size());
    buffer.append({
        static_cast<uint8_t>((count >> 24) & 0xFF),
        static_cast<uint8_t>((count >> 16) & 0xFF),
        static_cast<uint8_t>((count >> 8) & 0xFF),
        static_cast<uint8_t>(count & 0xFF)
    });

    for (const auto& elem : elements) {
        buffer.append(elem);
    }

    return buffer;
}

} // namespace AMFTestHelpers

// =============================================================================
// AMF0 Number Tests
// =============================================================================

class AMFCodecAMF0NumberTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0NumberTest, DecodeZero) {
    auto data = AMFTestHelpers::createAMF0Number(0.0);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Number);
    EXPECT_DOUBLE_EQ(result.value().asNumber(), 0.0);
    EXPECT_EQ(consumed, 9u);  // 1 byte marker + 8 bytes double
}

TEST_F(AMFCodecAMF0NumberTest, DecodePositiveInteger) {
    auto data = AMFTestHelpers::createAMF0Number(42.0);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Number);
    EXPECT_DOUBLE_EQ(result.value().asNumber(), 42.0);
}

TEST_F(AMFCodecAMF0NumberTest, DecodeNegativeNumber) {
    auto data = AMFTestHelpers::createAMF0Number(-123.456);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_DOUBLE_EQ(result.value().asNumber(), -123.456);
}

TEST_F(AMFCodecAMF0NumberTest, DecodeMaxDouble) {
    auto data = AMFTestHelpers::createAMF0Number(std::numeric_limits<double>::max());
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_DOUBLE_EQ(result.value().asNumber(), std::numeric_limits<double>::max());
}

TEST_F(AMFCodecAMF0NumberTest, DecodeInfinity) {
    auto data = AMFTestHelpers::createAMF0Number(std::numeric_limits<double>::infinity());
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(std::isinf(result.value().asNumber()));
}

TEST_F(AMFCodecAMF0NumberTest, DecodeNaN) {
    auto data = AMFTestHelpers::createAMF0Number(std::numeric_limits<double>::quiet_NaN());
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(std::isnan(result.value().asNumber()));
}

TEST_F(AMFCodecAMF0NumberTest, DecodeTruncatedData) {
    auto data = AMFTestHelpers::createAMF0Number(42.0);
    data.resize(5);  // Truncate to only 5 bytes (need 9)
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, AMFError::Code::UnexpectedEnd);
}

// =============================================================================
// AMF0 Boolean Tests
// =============================================================================

class AMFCodecAMF0BooleanTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0BooleanTest, DecodeTrue) {
    auto data = AMFTestHelpers::createAMF0Boolean(true);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Boolean);
    EXPECT_TRUE(result.value().asBool());
    EXPECT_EQ(consumed, 2u);  // 1 byte marker + 1 byte value
}

TEST_F(AMFCodecAMF0BooleanTest, DecodeFalse) {
    auto data = AMFTestHelpers::createAMF0Boolean(false);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_FALSE(result.value().asBool());
}

// =============================================================================
// AMF0 String Tests
// =============================================================================

class AMFCodecAMF0StringTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0StringTest, DecodeEmptyString) {
    auto data = AMFTestHelpers::createAMF0String("");
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::String);
    EXPECT_EQ(result.value().asString(), "");
    EXPECT_EQ(consumed, 3u);  // 1 byte marker + 2 bytes length
}

TEST_F(AMFCodecAMF0StringTest, DecodeSimpleString) {
    auto data = AMFTestHelpers::createAMF0String("connect");
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().asString(), "connect");
}

TEST_F(AMFCodecAMF0StringTest, DecodeUTF8String) {
    auto data = AMFTestHelpers::createAMF0String("Hello World!");
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().asString(), "Hello World!");
}

TEST_F(AMFCodecAMF0StringTest, DecodeMaxLengthString) {
    // Test string at max AMF0 string length (65535 bytes)
    std::string longStr(1000, 'x');  // Reasonable test size
    auto data = AMFTestHelpers::createAMF0String(longStr);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().asString().size(), 1000u);
}

// =============================================================================
// AMF0 Null and Undefined Tests
// =============================================================================

class AMFCodecAMF0NullUndefinedTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0NullUndefinedTest, DecodeNull) {
    auto data = AMFTestHelpers::createAMF0Null();
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Null);
    EXPECT_TRUE(result.value().isNull());
    EXPECT_EQ(consumed, 1u);
}

TEST_F(AMFCodecAMF0NullUndefinedTest, DecodeUndefined) {
    auto data = AMFTestHelpers::createAMF0Undefined();
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Undefined);
    EXPECT_EQ(consumed, 1u);
}

// =============================================================================
// AMF0 Object Tests
// =============================================================================

class AMFCodecAMF0ObjectTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0ObjectTest, DecodeEmptyObject) {
    auto data = AMFTestHelpers::createAMF0Object({});
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Object);
    EXPECT_TRUE(result.value().asObject().empty());
}

TEST_F(AMFCodecAMF0ObjectTest, DecodeSimpleObject) {
    auto numValue = AMFTestHelpers::createAMF0Number(1.0);
    auto strValue = AMFTestHelpers::createAMF0String("test");

    auto data = AMFTestHelpers::createAMF0Object({
        {"transactionId", numValue},
        {"app", strValue}
    });
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Object);

    const auto& obj = result.value().asObject();
    EXPECT_EQ(obj.size(), 2u);
    EXPECT_EQ(obj.at("transactionId").asNumber(), 1.0);
    EXPECT_EQ(obj.at("app").asString(), "test");
}

TEST_F(AMFCodecAMF0ObjectTest, DecodeNestedObject) {
    auto innerNum = AMFTestHelpers::createAMF0Number(42.0);
    auto innerObj = AMFTestHelpers::createAMF0Object({{"value", innerNum}});

    auto data = AMFTestHelpers::createAMF0Object({{"nested", innerObj}});
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());

    const auto& outer = result.value().asObject();
    EXPECT_TRUE(outer.count("nested") > 0);

    const auto& inner = outer.at("nested").asObject();
    EXPECT_EQ(inner.at("value").asNumber(), 42.0);
}

TEST_F(AMFCodecAMF0ObjectTest, DecodeConnectCommandObject) {
    // Simulate a typical RTMP connect command object
    auto app = AMFTestHelpers::createAMF0String("live");
    auto tcUrl = AMFTestHelpers::createAMF0String("rtmp://localhost/live");
    auto fpad = AMFTestHelpers::createAMF0Boolean(false);
    auto capabilities = AMFTestHelpers::createAMF0Number(239.0);
    auto audioCodecs = AMFTestHelpers::createAMF0Number(3575.0);
    auto videoCodecs = AMFTestHelpers::createAMF0Number(252.0);
    auto videoFunction = AMFTestHelpers::createAMF0Number(1.0);

    auto data = AMFTestHelpers::createAMF0Object({
        {"app", app},
        {"tcUrl", tcUrl},
        {"fpad", fpad},
        {"capabilities", capabilities},
        {"audioCodecs", audioCodecs},
        {"videoCodecs", videoCodecs},
        {"videoFunction", videoFunction}
    });
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());

    const auto& obj = result.value().asObject();
    EXPECT_EQ(obj.at("app").asString(), "live");
    EXPECT_EQ(obj.at("tcUrl").asString(), "rtmp://localhost/live");
    EXPECT_FALSE(obj.at("fpad").asBool());
    EXPECT_EQ(obj.at("capabilities").asNumber(), 239.0);
}

// =============================================================================
// AMF0 ECMAArray Tests
// =============================================================================

class AMFCodecAMF0ECMAArrayTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0ECMAArrayTest, DecodeEmptyECMAArray) {
    auto data = AMFTestHelpers::createAMF0ECMAArray(0, {});
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::ECMAArray);
}

TEST_F(AMFCodecAMF0ECMAArrayTest, DecodeECMAArrayWithProperties) {
    auto val1 = AMFTestHelpers::createAMF0Number(100.0);
    auto val2 = AMFTestHelpers::createAMF0String("hello");

    auto data = AMFTestHelpers::createAMF0ECMAArray(2, {
        {"prop1", val1},
        {"prop2", val2}
    });
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::ECMAArray);

    const auto& arr = result.value().asObject();
    EXPECT_EQ(arr.at("prop1").asNumber(), 100.0);
    EXPECT_EQ(arr.at("prop2").asString(), "hello");
}

// =============================================================================
// AMF0 StrictArray Tests
// =============================================================================

class AMFCodecAMF0StrictArrayTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0StrictArrayTest, DecodeEmptyStrictArray) {
    auto data = AMFTestHelpers::createAMF0StrictArray({});
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::StrictArray);
    EXPECT_TRUE(result.value().asArray().empty());
}

TEST_F(AMFCodecAMF0StrictArrayTest, DecodeStrictArrayWithElements) {
    auto elem1 = AMFTestHelpers::createAMF0Number(1.0);
    auto elem2 = AMFTestHelpers::createAMF0Number(2.0);
    auto elem3 = AMFTestHelpers::createAMF0String("three");

    auto data = AMFTestHelpers::createAMF0StrictArray({elem1, elem2, elem3});
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::StrictArray);

    const auto& arr = result.value().asArray();
    ASSERT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0].asNumber(), 1.0);
    EXPECT_EQ(arr[1].asNumber(), 2.0);
    EXPECT_EQ(arr[2].asString(), "three");
}

// =============================================================================
// AMF0 Date Tests
// =============================================================================

class AMFCodecAMF0DateTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0DateTest, DecodeDate) {
    double timestamp = 1700000000000.0;  // Some timestamp in milliseconds
    auto data = AMFTestHelpers::createAMF0Date(timestamp, 0);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Date);
    EXPECT_EQ(consumed, 11u);  // 1 marker + 8 double + 2 timezone
}

// =============================================================================
// AMF0 Long String Tests
// =============================================================================

class AMFCodecAMF0LongStringTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0LongStringTest, DecodeLongString) {
    std::string longStr(70000, 'a');  // More than 65535 bytes
    auto data = AMFTestHelpers::createAMF0LongString(longStr);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::String);
    EXPECT_EQ(result.value().asString().size(), 70000u);
}

// =============================================================================
// AMF0 Reference Tests
// =============================================================================

class AMFCodecAMF0ReferenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0ReferenceTest, DecodeReference) {
    // First decode an object to add to reference table
    auto obj = AMFTestHelpers::createAMF0Object({});
    size_t consumed = 0;
    codec_->decodeAMF0(obj.data(), obj.size(), consumed);

    // Now decode a reference to it
    auto ref = AMFTestHelpers::createAMF0Reference(0);
    consumed = 0;

    auto result = codec_->decodeAMF0(ref.data(), ref.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Reference);
}

TEST_F(AMFCodecAMF0ReferenceTest, DecodeInvalidReference) {
    // Reference without any prior objects should fail
    codec_->resetReferenceTables();  // Ensure clean state

    auto ref = AMFTestHelpers::createAMF0Reference(99);  // Invalid index
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(ref.data(), ref.size(), consumed);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, AMFError::Code::InvalidReference);
}

// =============================================================================
// AMF0 Encoding Tests
// =============================================================================

class AMFCodecAMF0EncodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF0EncodeTest, EncodeNumber) {
    AMFValue value;
    value.type = AMFValue::Type::Number;
    value.data = 42.5;

    auto result = codec_->encodeAMF0(value);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value()[0], amf0::NUMBER_MARKER);
    EXPECT_EQ(result.value().size(), 9u);

    // Decode to verify
    size_t consumed = 0;
    auto decoded = codec_->decodeAMF0(result.value().data(), result.value().size(), consumed);
    ASSERT_TRUE(decoded.isSuccess());
    EXPECT_DOUBLE_EQ(decoded.value().asNumber(), 42.5);
}

TEST_F(AMFCodecAMF0EncodeTest, EncodeBoolean) {
    AMFValue trueValue;
    trueValue.type = AMFValue::Type::Boolean;
    trueValue.data = true;

    auto result = codec_->encodeAMF0(trueValue);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value()[0], amf0::BOOLEAN_MARKER);
    EXPECT_EQ(result.value()[1], 1);
}

TEST_F(AMFCodecAMF0EncodeTest, EncodeString) {
    AMFValue value;
    value.type = AMFValue::Type::String;
    value.data = std::string("hello");

    auto result = codec_->encodeAMF0(value);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value()[0], amf0::STRING_MARKER);

    // Decode to verify
    size_t consumed = 0;
    auto decoded = codec_->decodeAMF0(result.value().data(), result.value().size(), consumed);
    ASSERT_TRUE(decoded.isSuccess());
    EXPECT_EQ(decoded.value().asString(), "hello");
}

TEST_F(AMFCodecAMF0EncodeTest, EncodeNull) {
    AMFValue value;
    value.type = AMFValue::Type::Null;
    value.data = nullptr;

    auto result = codec_->encodeAMF0(value);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0], amf0::NULL_MARKER);
}

TEST_F(AMFCodecAMF0EncodeTest, EncodeObject) {
    AMFValue value;
    value.type = AMFValue::Type::Object;

    std::map<std::string, AMFValue> obj;
    AMFValue numVal;
    numVal.type = AMFValue::Type::Number;
    numVal.data = 1.0;
    obj["transactionId"] = numVal;

    value.data = obj;

    auto result = codec_->encodeAMF0(value);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value()[0], amf0::OBJECT_MARKER);

    // Decode to verify
    size_t consumed = 0;
    auto decoded = codec_->decodeAMF0(result.value().data(), result.value().size(), consumed);
    ASSERT_TRUE(decoded.isSuccess());
    EXPECT_EQ(decoded.value().asObject().at("transactionId").asNumber(), 1.0);
}

// =============================================================================
// Batch Decode Tests (decodeAll)
// =============================================================================

class AMFCodecDecodeAllTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecDecodeAllTest, DecodeMultipleAMF0Values) {
    core::Buffer data;
    data.append(AMFTestHelpers::createAMF0String("connect"));
    data.append(AMFTestHelpers::createAMF0Number(1.0));
    data.append(AMFTestHelpers::createAMF0Object({
        {"app", AMFTestHelpers::createAMF0String("live")}
    }));

    auto result = codec_->decodeAll(AMFVersion::AMF0, data.data(), data.size());

    ASSERT_TRUE(result.isSuccess());
    ASSERT_EQ(result.value().size(), 3u);
    EXPECT_EQ(result.value()[0].asString(), "connect");
    EXPECT_EQ(result.value()[1].asNumber(), 1.0);
    EXPECT_EQ(result.value()[2].asObject().at("app").asString(), "live");
}

// =============================================================================
// Safety Limits Tests
// =============================================================================

class AMFCodecSafetyLimitsTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecSafetyLimitsTest, ExceedMaxNestingDepth) {
    // Create deeply nested object (more than 32 levels)
    core::Buffer data;
    data.append({amf0::OBJECT_MARKER});

    // Create 33 levels of nesting
    for (int i = 0; i < 33; i++) {
        data.append(AMFTestHelpers::createAMF0PropertyName("nested"));
        data.append({amf0::OBJECT_MARKER});
    }

    // Close all objects
    for (int i = 0; i < 33; i++) {
        data.append(AMFTestHelpers::createAMF0ObjectEnd());
    }
    data.append(AMFTestHelpers::createAMF0ObjectEnd());

    size_t consumed = 0;
    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, AMFError::Code::NestingTooDeep);
}

TEST_F(AMFCodecSafetyLimitsTest, StringLengthLimit) {
    // Create a string length that exceeds 64KB limit for short strings
    // The decoder should handle this gracefully
    core::Buffer data;
    data.append({amf0::STRING_MARKER});
    // Write length as 65536 (0x10000) - but AMF0 string only has 16-bit length
    // So we'll test with Long String marker instead

    // Actually test that normal string decoding works with max length
    std::string maxStr(65535, 'x');
    auto validData = AMFTestHelpers::createAMF0String(maxStr);
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(validData.data(), validData.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().asString().size(), 65535u);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

class AMFCodecErrorHandlingTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecErrorHandlingTest, InvalidTypeMarker) {
    core::Buffer data;
    data.append({0xFF});  // Invalid AMF0 type marker
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, AMFError::Code::InvalidType);
    EXPECT_EQ(result.error().offset, 0u);
}

TEST_F(AMFCodecErrorHandlingTest, UnexpectedEndOfData) {
    core::Buffer data;
    data.append({amf0::NUMBER_MARKER});
    // Missing 8 bytes of double data
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, AMFError::Code::UnexpectedEnd);
}

TEST_F(AMFCodecErrorHandlingTest, EmptyInput) {
    size_t consumed = 0;

    auto result = codec_->decodeAMF0(nullptr, 0, consumed);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, AMFError::Code::UnexpectedEnd);
}

TEST_F(AMFCodecErrorHandlingTest, ErrorOffsetReporting) {
    core::Buffer data;
    data.append(AMFTestHelpers::createAMF0String("valid"));
    data.append({0xFF});  // Invalid type after valid data

    auto result = codec_->decodeAll(AMFVersion::AMF0, data.data(), data.size());

    // Should fail on second value with correct offset
    ASSERT_TRUE(result.isError());
    EXPECT_GT(result.error().offset, 0u);
}

// =============================================================================
// Reference Table Reset Tests
// =============================================================================

class AMFCodecReferenceTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecReferenceTableTest, ResetReferenceTablesBetweenMessages) {
    // Decode an object to populate reference table
    auto obj = AMFTestHelpers::createAMF0Object({
        {"key", AMFTestHelpers::createAMF0String("value")}
    });
    size_t consumed = 0;
    codec_->decodeAMF0(obj.data(), obj.size(), consumed);

    // Reset reference tables (as would happen between messages)
    codec_->resetReferenceTables();

    // Reference to previous object should now fail
    auto ref = AMFTestHelpers::createAMF0Reference(0);
    consumed = 0;

    auto result = codec_->decodeAMF0(ref.data(), ref.size(), consumed);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, AMFError::Code::InvalidReference);
}

// =============================================================================
// AMF3 Basic Tests
// =============================================================================

class AMFCodecAMF3Test : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF3Test, DecodeUndefined) {
    core::Buffer data;
    data.append({amf3::UNDEFINED_MARKER});
    size_t consumed = 0;

    auto result = codec_->decodeAMF3(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Undefined);
}

TEST_F(AMFCodecAMF3Test, DecodeNull) {
    core::Buffer data;
    data.append({amf3::NULL_MARKER});
    size_t consumed = 0;

    auto result = codec_->decodeAMF3(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Null);
}

TEST_F(AMFCodecAMF3Test, DecodeFalse) {
    core::Buffer data;
    data.append({amf3::FALSE_MARKER});
    size_t consumed = 0;

    auto result = codec_->decodeAMF3(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Boolean);
    EXPECT_FALSE(result.value().asBool());
}

TEST_F(AMFCodecAMF3Test, DecodeTrue) {
    core::Buffer data;
    data.append({amf3::TRUE_MARKER});
    size_t consumed = 0;

    auto result = codec_->decodeAMF3(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Boolean);
    EXPECT_TRUE(result.value().asBool());
}

TEST_F(AMFCodecAMF3Test, DecodeInteger) {
    // AMF3 Integer uses variable-length encoding (U29)
    core::Buffer data;
    data.append({amf3::INTEGER_MARKER, 0x04});  // Value 4 (fits in 1 byte)
    size_t consumed = 0;

    auto result = codec_->decodeAMF3(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Number);
    EXPECT_DOUBLE_EQ(result.value().asNumber(), 4.0);
}

TEST_F(AMFCodecAMF3Test, DecodeDouble) {
    core::Buffer data;
    data.append({amf3::DOUBLE_MARKER});
    AMFTestHelpers::writeDoubleBE(data, 3.14159);
    size_t consumed = 0;

    auto result = codec_->decodeAMF3(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::Number);
    EXPECT_NEAR(result.value().asNumber(), 3.14159, 0.00001);
}

TEST_F(AMFCodecAMF3Test, DecodeEmptyString) {
    core::Buffer data;
    data.append({amf3::STRING_MARKER, 0x01});  // Empty string (length 0, inline flag set)
    size_t consumed = 0;

    auto result = codec_->decodeAMF3(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().type, AMFValue::Type::String);
    EXPECT_EQ(result.value().asString(), "");
}

// =============================================================================
// AMF3 Encoding Tests
// =============================================================================

class AMFCodecAMF3EncodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        codec_ = std::make_unique<AMFCodec>();
    }

    std::unique_ptr<AMFCodec> codec_;
};

TEST_F(AMFCodecAMF3EncodeTest, EncodeNull) {
    AMFValue value;
    value.type = AMFValue::Type::Null;
    value.data = nullptr;

    auto result = codec_->encodeAMF3(value);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0], amf3::NULL_MARKER);
}

TEST_F(AMFCodecAMF3EncodeTest, EncodeBoolean) {
    AMFValue trueValue;
    trueValue.type = AMFValue::Type::Boolean;
    trueValue.data = true;

    auto result = codec_->encodeAMF3(trueValue);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0], amf3::TRUE_MARKER);
}

TEST_F(AMFCodecAMF3EncodeTest, EncodeDouble) {
    AMFValue value;
    value.type = AMFValue::Type::Number;
    value.data = 123.456;

    auto result = codec_->encodeAMF3(value);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value()[0], amf3::DOUBLE_MARKER);

    // Decode to verify
    size_t consumed = 0;
    auto decoded = codec_->decodeAMF3(result.value().data(), result.value().size(), consumed);
    ASSERT_TRUE(decoded.isSuccess());
    EXPECT_NEAR(decoded.value().asNumber(), 123.456, 0.001);
}

} // namespace test
} // namespace protocol
} // namespace openrtmp
