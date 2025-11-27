// OpenRTMP - Cross-platform RTMP Server
// AMF Codec Implementation
//
// Implements AMF0 and AMF3 encoding/decoding per Adobe specifications.
//
// Requirements coverage:
// - Requirements 3.1-3.7: RTMP command processing depends on AMF codec

#include "openrtmp/protocol/amf_codec.hpp"
#include <cstring>
#include <cmath>

namespace openrtmp {
namespace protocol {

// =============================================================================
// Constructor
// =============================================================================

AMFCodec::AMFCodec() {
    // Reserve some space for reference tables
    amf0ObjectReferences_.reserve(16);
    amf3StringReferences_.reserve(32);
    amf3ObjectReferences_.reserve(16);
}

// =============================================================================
// Reference Table Management
// =============================================================================

void AMFCodec::resetReferenceTables() {
    amf0ObjectReferences_.clear();
    amf3StringReferences_.clear();
    amf3ObjectReferences_.clear();
}

// =============================================================================
// Utility Functions
// =============================================================================

double AMFCodec::readDoubleBE(const uint8_t* data) {
    uint64_t bits = 0;
    bits |= static_cast<uint64_t>(data[0]) << 56;
    bits |= static_cast<uint64_t>(data[1]) << 48;
    bits |= static_cast<uint64_t>(data[2]) << 40;
    bits |= static_cast<uint64_t>(data[3]) << 32;
    bits |= static_cast<uint64_t>(data[4]) << 24;
    bits |= static_cast<uint64_t>(data[5]) << 16;
    bits |= static_cast<uint64_t>(data[6]) << 8;
    bits |= static_cast<uint64_t>(data[7]);

    double result;
    std::memcpy(&result, &bits, sizeof(double));
    return result;
}

void AMFCodec::writeDoubleBE(std::vector<uint8_t>& buffer, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));

    buffer.push_back(static_cast<uint8_t>((bits >> 56) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((bits >> 48) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((bits >> 40) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((bits >> 32) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(bits & 0xFF));
}

// =============================================================================
// AMF0 Decoding
// =============================================================================

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed
) {
    return decodeAMF0Value(data, length, bytesConsumed, 0, 0);
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0Value(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t depth,
    size_t baseOffset
) {
    // Check nesting depth limit
    if (depth > amf_limits::MAX_NESTING_DEPTH) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::NestingTooDeep, baseOffset,
                     "Maximum nesting depth exceeded"));
    }

    // Need at least 1 byte for type marker
    if (length < 1 || data == nullptr) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset,
                     "No data available for AMF0 value"));
    }

    uint8_t marker = data[0];
    bytesConsumed = 1;  // At minimum, we consume the marker

    switch (marker) {
        case amf0::NUMBER_MARKER:
            return decodeAMF0Number(data + 1, length - 1, bytesConsumed, baseOffset);

        case amf0::BOOLEAN_MARKER:
            return decodeAMF0Boolean(data + 1, length - 1, bytesConsumed, baseOffset);

        case amf0::STRING_MARKER:
            return decodeAMF0String(data + 1, length - 1, bytesConsumed, baseOffset);

        case amf0::OBJECT_MARKER:
            return decodeAMF0Object(data + 1, length - 1, bytesConsumed, depth, baseOffset);

        case amf0::NULL_MARKER: {
            AMFValue value;
            value.type = AMFValue::Type::Null;
            value.data = nullptr;
            return core::Result<AMFValue, AMFError>::success(value);
        }

        case amf0::UNDEFINED_MARKER: {
            AMFValue value;
            value.type = AMFValue::Type::Undefined;
            value.data = nullptr;
            return core::Result<AMFValue, AMFError>::success(value);
        }

        case amf0::REFERENCE_MARKER:
            return decodeAMF0Reference(data + 1, length - 1, bytesConsumed, baseOffset);

        case amf0::ECMA_ARRAY_MARKER:
            return decodeAMF0ECMAArray(data + 1, length - 1, bytesConsumed, depth, baseOffset);

        case amf0::STRICT_ARRAY_MARKER:
            return decodeAMF0StrictArray(data + 1, length - 1, bytesConsumed, depth, baseOffset);

        case amf0::DATE_MARKER:
            return decodeAMF0Date(data + 1, length - 1, bytesConsumed, baseOffset);

        case amf0::LONG_STRING_MARKER:
            return decodeAMF0LongString(data + 1, length - 1, bytesConsumed, baseOffset);

        case amf0::AVMPLUS_MARKER: {
            // Switch to AMF3 mode
            size_t amf3Consumed = 0;
            auto result = decodeAMF3Value(data + 1, length - 1, amf3Consumed, depth, baseOffset + 1);
            if (result.isSuccess()) {
                bytesConsumed += amf3Consumed;
            }
            return result;
        }

        default:
            return core::Result<AMFValue, AMFError>::error(
                AMFError(AMFError::Code::InvalidType, baseOffset,
                         "Unknown AMF0 type marker: " + std::to_string(marker)));
    }
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0Number(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 8) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for AMF0 number"));
    }

    double value = readDoubleBE(data);
    bytesConsumed += 8;

    AMFValue result;
    result.type = AMFValue::Type::Number;
    result.data = value;
    return core::Result<AMFValue, AMFError>::success(result);
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0Boolean(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 1) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for AMF0 boolean"));
    }

    bool value = (data[0] != 0);
    bytesConsumed += 1;

    AMFValue result;
    result.type = AMFValue::Type::Boolean;
    result.data = value;
    return core::Result<AMFValue, AMFError>::success(result);
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0String(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 2) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for AMF0 string length"));
    }

    uint16_t strLength = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) | data[1]
    );

    if (length < 2 + strLength) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 3,
                     "Not enough data for AMF0 string content"));
    }

    std::string str(reinterpret_cast<const char*>(data + 2), strLength);
    bytesConsumed += 2 + strLength;

    AMFValue result;
    result.type = AMFValue::Type::String;
    result.data = std::move(str);
    return core::Result<AMFValue, AMFError>::success(result);
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0LongString(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 4) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for AMF0 long string length"));
    }

    uint32_t strLength =
        (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);

    if (length < 4 + strLength) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 5,
                     "Not enough data for AMF0 long string content"));
    }

    std::string str(reinterpret_cast<const char*>(data + 4), strLength);
    bytesConsumed += 4 + strLength;

    AMFValue result;
    result.type = AMFValue::Type::String;
    result.data = std::move(str);
    return core::Result<AMFValue, AMFError>::success(result);
}

core::Result<std::string, AMFError> AMFCodec::decodeAMF0ShortString(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 2) {
        return core::Result<std::string, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset,
                     "Not enough data for property name length"));
    }

    uint16_t strLength = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) | data[1]
    );

    if (length < 2 + strLength) {
        return core::Result<std::string, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 2,
                     "Not enough data for property name content"));
    }

    std::string str(reinterpret_cast<const char*>(data + 2), strLength);
    bytesConsumed = 2 + strLength;

    return core::Result<std::string, AMFError>::success(std::move(str));
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0Object(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t depth,
    size_t baseOffset
) {
    std::map<std::string, AMFValue> properties;
    size_t offset = 0;

    while (offset < length) {
        // Check for object end marker (00 00 09)
        if (length - offset >= 3 &&
            data[offset] == 0x00 && data[offset + 1] == 0x00 &&
            data[offset + 2] == amf0::OBJECT_END_MARKER) {
            bytesConsumed += offset + 3;

            AMFValue result;
            result.type = AMFValue::Type::Object;
            result.data = std::move(properties);

            // Add to reference table
            amf0ObjectReferences_.push_back(result);

            return core::Result<AMFValue, AMFError>::success(result);
        }

        // Decode property name
        size_t nameConsumed = 0;
        auto nameResult = decodeAMF0ShortString(
            data + offset, length - offset, nameConsumed, baseOffset + 1 + offset);

        if (nameResult.isError()) {
            return core::Result<AMFValue, AMFError>::error(nameResult.error());
        }

        offset += nameConsumed;

        // Decode property value
        size_t valueConsumed = 0;
        auto valueResult = decodeAMF0Value(
            data + offset, length - offset, valueConsumed, depth + 1, baseOffset + 1 + offset);

        if (valueResult.isError()) {
            return core::Result<AMFValue, AMFError>::error(valueResult.error());
        }

        properties[nameResult.value()] = valueResult.value();
        offset += valueConsumed;
    }

    return core::Result<AMFValue, AMFError>::error(
        AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1 + offset,
                 "Object end marker not found"));
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0ECMAArray(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t depth,
    size_t baseOffset
) {
    if (length < 4) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for ECMA array count"));
    }

    // Skip the approximate count (4 bytes) - it's just a hint
    size_t offset = 4;
    std::map<std::string, AMFValue> properties;

    while (offset < length) {
        // Check for object end marker
        if (length - offset >= 3 &&
            data[offset] == 0x00 && data[offset + 1] == 0x00 &&
            data[offset + 2] == amf0::OBJECT_END_MARKER) {
            bytesConsumed += offset + 3;

            AMFValue result;
            result.type = AMFValue::Type::ECMAArray;
            result.data = std::move(properties);

            amf0ObjectReferences_.push_back(result);

            return core::Result<AMFValue, AMFError>::success(result);
        }

        // Decode property name
        size_t nameConsumed = 0;
        auto nameResult = decodeAMF0ShortString(
            data + offset, length - offset, nameConsumed, baseOffset + 1 + offset);

        if (nameResult.isError()) {
            return core::Result<AMFValue, AMFError>::error(nameResult.error());
        }

        offset += nameConsumed;

        // Decode property value
        size_t valueConsumed = 0;
        auto valueResult = decodeAMF0Value(
            data + offset, length - offset, valueConsumed, depth + 1, baseOffset + 1 + offset);

        if (valueResult.isError()) {
            return core::Result<AMFValue, AMFError>::error(valueResult.error());
        }

        properties[nameResult.value()] = valueResult.value();
        offset += valueConsumed;
    }

    return core::Result<AMFValue, AMFError>::error(
        AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1 + offset,
                 "ECMA array end marker not found"));
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0StrictArray(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t depth,
    size_t baseOffset
) {
    if (length < 4) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for strict array count"));
    }

    uint32_t count =
        (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);

    size_t offset = 4;
    std::vector<AMFValue> elements;
    elements.reserve(count);

    for (uint32_t i = 0; i < count && offset < length; i++) {
        size_t valueConsumed = 0;
        auto valueResult = decodeAMF0Value(
            data + offset, length - offset, valueConsumed, depth + 1, baseOffset + 1 + offset);

        if (valueResult.isError()) {
            return core::Result<AMFValue, AMFError>::error(valueResult.error());
        }

        elements.push_back(valueResult.value());
        offset += valueConsumed;
    }

    bytesConsumed += offset;

    AMFValue result;
    result.type = AMFValue::Type::StrictArray;
    result.data = std::move(elements);

    amf0ObjectReferences_.push_back(result);

    return core::Result<AMFValue, AMFError>::success(result);
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0Date(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 10) {  // 8 bytes double + 2 bytes timezone
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for AMF0 date"));
    }

    double timestamp = readDoubleBE(data);
    // int16_t timezone = (data[8] << 8) | data[9];  // Timezone offset (unused in practice)
    bytesConsumed += 10;

    AMFValue result;
    result.type = AMFValue::Type::Date;
    result.data = std::chrono::milliseconds(static_cast<int64_t>(timestamp));
    return core::Result<AMFValue, AMFError>::success(result);
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF0Reference(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 2) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                     "Not enough data for AMF0 reference"));
    }

    uint16_t index = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) | data[1]
    );

    if (index >= amf0ObjectReferences_.size()) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::InvalidReference, baseOffset + 1,
                     "Reference index out of bounds: " + std::to_string(index)));
    }

    bytesConsumed += 2;

    AMFValue result;
    result.type = AMFValue::Type::Reference;
    result.data = index;
    return core::Result<AMFValue, AMFError>::success(result);
}

// =============================================================================
// AMF0 Encoding
// =============================================================================

core::Result<std::vector<uint8_t>, AMFError> AMFCodec::encodeAMF0(const AMFValue& value) {
    std::vector<uint8_t> buffer;
    buffer.reserve(128);

    try {
        encodeAMF0Value(buffer, value);
        return core::Result<std::vector<uint8_t>, AMFError>::success(std::move(buffer));
    } catch (const std::exception& e) {
        return core::Result<std::vector<uint8_t>, AMFError>::error(
            AMFError(AMFError::Code::EncodingFailed, 0, e.what()));
    }
}

void AMFCodec::encodeAMF0Value(std::vector<uint8_t>& buffer, const AMFValue& value) {
    switch (value.type) {
        case AMFValue::Type::Null:
            buffer.push_back(amf0::NULL_MARKER);
            break;

        case AMFValue::Type::Undefined:
            buffer.push_back(amf0::UNDEFINED_MARKER);
            break;

        case AMFValue::Type::Boolean:
            encodeAMF0Boolean(buffer, value.asBool());
            break;

        case AMFValue::Type::Number:
            encodeAMF0Number(buffer, value.asNumber());
            break;

        case AMFValue::Type::String:
            encodeAMF0String(buffer, value.asString());
            break;

        case AMFValue::Type::Object:
        case AMFValue::Type::ECMAArray:
            encodeAMF0Object(buffer, value.asObject());
            break;

        case AMFValue::Type::StrictArray:
        case AMFValue::Type::Array:
            encodeAMF0Array(buffer, value.asArray());
            break;

        default:
            buffer.push_back(amf0::NULL_MARKER);
            break;
    }
}

void AMFCodec::encodeAMF0Number(std::vector<uint8_t>& buffer, double value) {
    buffer.push_back(amf0::NUMBER_MARKER);
    writeDoubleBE(buffer, value);
}

void AMFCodec::encodeAMF0Boolean(std::vector<uint8_t>& buffer, bool value) {
    buffer.push_back(amf0::BOOLEAN_MARKER);
    buffer.push_back(value ? 1 : 0);
}

void AMFCodec::encodeAMF0String(std::vector<uint8_t>& buffer, const std::string& value) {
    if (value.size() > 65535) {
        // Use long string for strings > 65535 bytes
        buffer.push_back(amf0::LONG_STRING_MARKER);
        uint32_t length = static_cast<uint32_t>(value.size());
        buffer.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        buffer.push_back(static_cast<uint8_t>(length & 0xFF));
    } else {
        buffer.push_back(amf0::STRING_MARKER);
        uint16_t length = static_cast<uint16_t>(value.size());
        buffer.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        buffer.push_back(static_cast<uint8_t>(length & 0xFF));
    }
    buffer.insert(buffer.end(), value.begin(), value.end());
}

void AMFCodec::encodeAMF0Object(std::vector<uint8_t>& buffer, const std::map<std::string, AMFValue>& obj) {
    buffer.push_back(amf0::OBJECT_MARKER);

    for (const auto& [key, val] : obj) {
        // Encode property name (short string without marker)
        uint16_t keyLen = static_cast<uint16_t>(key.size());
        buffer.push_back(static_cast<uint8_t>((keyLen >> 8) & 0xFF));
        buffer.push_back(static_cast<uint8_t>(keyLen & 0xFF));
        buffer.insert(buffer.end(), key.begin(), key.end());

        // Encode property value
        encodeAMF0Value(buffer, val);
    }

    // Object end marker
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(amf0::OBJECT_END_MARKER);
}

void AMFCodec::encodeAMF0Array(std::vector<uint8_t>& buffer, const std::vector<AMFValue>& arr) {
    buffer.push_back(amf0::STRICT_ARRAY_MARKER);

    uint32_t count = static_cast<uint32_t>(arr.size());
    buffer.push_back(static_cast<uint8_t>((count >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((count >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(count & 0xFF));

    for (const auto& elem : arr) {
        encodeAMF0Value(buffer, elem);
    }
}

// =============================================================================
// AMF3 Decoding
// =============================================================================

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF3(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed
) {
    return decodeAMF3Value(data, length, bytesConsumed, 0, 0);
}

core::Result<AMFValue, AMFError> AMFCodec::decodeAMF3Value(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t depth,
    size_t baseOffset
) {
    if (depth > amf_limits::MAX_NESTING_DEPTH) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::NestingTooDeep, baseOffset,
                     "Maximum nesting depth exceeded"));
    }

    if (length < 1 || data == nullptr) {
        return core::Result<AMFValue, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset,
                     "No data available for AMF3 value"));
    }

    uint8_t marker = data[0];
    bytesConsumed = 1;

    switch (marker) {
        case amf3::UNDEFINED_MARKER: {
            AMFValue result;
            result.type = AMFValue::Type::Undefined;
            result.data = nullptr;
            return core::Result<AMFValue, AMFError>::success(result);
        }

        case amf3::NULL_MARKER: {
            AMFValue result;
            result.type = AMFValue::Type::Null;
            result.data = nullptr;
            return core::Result<AMFValue, AMFError>::success(result);
        }

        case amf3::FALSE_MARKER: {
            AMFValue result;
            result.type = AMFValue::Type::Boolean;
            result.data = false;
            return core::Result<AMFValue, AMFError>::success(result);
        }

        case amf3::TRUE_MARKER: {
            AMFValue result;
            result.type = AMFValue::Type::Boolean;
            result.data = true;
            return core::Result<AMFValue, AMFError>::success(result);
        }

        case amf3::INTEGER_MARKER: {
            size_t u29Consumed = 0;
            auto u29Result = decodeU29(data + 1, length - 1, u29Consumed, baseOffset + 1);
            if (u29Result.isError()) {
                return core::Result<AMFValue, AMFError>::error(u29Result.error());
            }

            bytesConsumed += u29Consumed;

            // AMF3 integers are signed 29-bit values
            int32_t value = static_cast<int32_t>(u29Result.value());
            if (value >= 0x10000000) {
                value -= 0x20000000;  // Convert to signed
            }

            AMFValue result;
            result.type = AMFValue::Type::Number;
            result.data = static_cast<double>(value);
            return core::Result<AMFValue, AMFError>::success(result);
        }

        case amf3::DOUBLE_MARKER: {
            if (length < 9) {
                return core::Result<AMFValue, AMFError>::error(
                    AMFError(AMFError::Code::UnexpectedEnd, baseOffset + 1,
                             "Not enough data for AMF3 double"));
            }

            double value = readDoubleBE(data + 1);
            bytesConsumed += 8;

            AMFValue result;
            result.type = AMFValue::Type::Number;
            result.data = value;
            return core::Result<AMFValue, AMFError>::success(result);
        }

        case amf3::STRING_MARKER: {
            size_t strConsumed = 0;
            auto strResult = decodeAMF3String(data + 1, length - 1, strConsumed, baseOffset + 1);
            if (strResult.isError()) {
                return core::Result<AMFValue, AMFError>::error(strResult.error());
            }

            bytesConsumed += strConsumed;

            AMFValue result;
            result.type = AMFValue::Type::String;
            result.data = std::move(strResult.value());
            return core::Result<AMFValue, AMFError>::success(result);
        }

        // TODO: Implement remaining AMF3 types (array, object, date, etc.)
        // For now, return error for unsupported types

        default:
            return core::Result<AMFValue, AMFError>::error(
                AMFError(AMFError::Code::InvalidType, baseOffset,
                         "Unsupported AMF3 type marker: " + std::to_string(marker)));
    }
}

core::Result<uint32_t, AMFError> AMFCodec::decodeU29(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    if (length < 1) {
        return core::Result<uint32_t, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset,
                     "No data for U29 encoding"));
    }

    uint32_t result = 0;
    bytesConsumed = 0;

    // U29 uses variable-length encoding:
    // - If bit 7 is 0, the value is complete (7 bits)
    // - If bit 7 is 1, continue to next byte
    // - Maximum 4 bytes, last byte uses all 8 bits

    for (int i = 0; i < 4 && bytesConsumed < length; i++) {
        uint8_t byte = data[bytesConsumed++];

        if (i < 3) {
            result = (result << 7) | (byte & 0x7F);
            if ((byte & 0x80) == 0) {
                return core::Result<uint32_t, AMFError>::success(result);
            }
        } else {
            // Fourth byte uses all 8 bits
            result = (result << 8) | byte;
            return core::Result<uint32_t, AMFError>::success(result);
        }
    }

    return core::Result<uint32_t, AMFError>::error(
        AMFError(AMFError::Code::UnexpectedEnd, baseOffset + bytesConsumed,
                 "Incomplete U29 encoding"));
}

core::Result<std::string, AMFError> AMFCodec::decodeAMF3String(
    const uint8_t* data,
    size_t length,
    size_t& bytesConsumed,
    size_t baseOffset
) {
    size_t u29Consumed = 0;
    auto u29Result = decodeU29(data, length, u29Consumed, baseOffset);
    if (u29Result.isError()) {
        return core::Result<std::string, AMFError>::error(u29Result.error());
    }

    uint32_t u29 = u29Result.value();
    bytesConsumed = u29Consumed;

    if ((u29 & 0x01) == 0) {
        // Reference to string table
        uint32_t index = u29 >> 1;
        if (index >= amf3StringReferences_.size()) {
            return core::Result<std::string, AMFError>::error(
                AMFError(AMFError::Code::InvalidReference, baseOffset,
                         "AMF3 string reference out of bounds"));
        }
        return core::Result<std::string, AMFError>::success(amf3StringReferences_[index]);
    }

    // Inline string
    uint32_t strLength = u29 >> 1;

    if (length - bytesConsumed < strLength) {
        return core::Result<std::string, AMFError>::error(
            AMFError(AMFError::Code::UnexpectedEnd, baseOffset + bytesConsumed,
                     "Not enough data for AMF3 string content"));
    }

    std::string str(reinterpret_cast<const char*>(data + bytesConsumed), strLength);
    bytesConsumed += strLength;

    // Add non-empty strings to reference table
    if (!str.empty()) {
        amf3StringReferences_.push_back(str);
    }

    return core::Result<std::string, AMFError>::success(std::move(str));
}

// =============================================================================
// AMF3 Encoding
// =============================================================================

core::Result<std::vector<uint8_t>, AMFError> AMFCodec::encodeAMF3(const AMFValue& value) {
    std::vector<uint8_t> buffer;
    buffer.reserve(64);

    try {
        encodeAMF3Value(buffer, value);
        return core::Result<std::vector<uint8_t>, AMFError>::success(std::move(buffer));
    } catch (const std::exception& e) {
        return core::Result<std::vector<uint8_t>, AMFError>::error(
            AMFError(AMFError::Code::EncodingFailed, 0, e.what()));
    }
}

void AMFCodec::encodeAMF3Value(std::vector<uint8_t>& buffer, const AMFValue& value) {
    switch (value.type) {
        case AMFValue::Type::Undefined:
            buffer.push_back(amf3::UNDEFINED_MARKER);
            break;

        case AMFValue::Type::Null:
            buffer.push_back(amf3::NULL_MARKER);
            break;

        case AMFValue::Type::Boolean:
            buffer.push_back(value.asBool() ? amf3::TRUE_MARKER : amf3::FALSE_MARKER);
            break;

        case AMFValue::Type::Number: {
            double num = value.asNumber();
            // Check if value fits in 29-bit integer
            if (std::floor(num) == num &&
                num >= amf_limits::AMF3_INT_MIN &&
                num <= amf_limits::AMF3_INT_MAX) {
                buffer.push_back(amf3::INTEGER_MARKER);
                int32_t intVal = static_cast<int32_t>(num);
                if (intVal < 0) {
                    intVal += 0x20000000;  // Convert to unsigned representation
                }
                encodeU29(buffer, static_cast<uint32_t>(intVal));
            } else {
                buffer.push_back(amf3::DOUBLE_MARKER);
                writeDoubleBE(buffer, num);
            }
            break;
        }

        case AMFValue::Type::String:
            buffer.push_back(amf3::STRING_MARKER);
            encodeAMF3String(buffer, value.asString());
            break;

        default:
            buffer.push_back(amf3::NULL_MARKER);
            break;
    }
}

void AMFCodec::encodeU29(std::vector<uint8_t>& buffer, uint32_t value) {
    if (value < 0x80) {
        // 1 byte
        buffer.push_back(static_cast<uint8_t>(value));
    } else if (value < 0x4000) {
        // 2 bytes
        buffer.push_back(static_cast<uint8_t>((value >> 7) | 0x80));
        buffer.push_back(static_cast<uint8_t>(value & 0x7F));
    } else if (value < 0x200000) {
        // 3 bytes
        buffer.push_back(static_cast<uint8_t>((value >> 14) | 0x80));
        buffer.push_back(static_cast<uint8_t>((value >> 7) | 0x80));
        buffer.push_back(static_cast<uint8_t>(value & 0x7F));
    } else {
        // 4 bytes
        buffer.push_back(static_cast<uint8_t>((value >> 22) | 0x80));
        buffer.push_back(static_cast<uint8_t>((value >> 15) | 0x80));
        buffer.push_back(static_cast<uint8_t>((value >> 8) | 0x80));
        buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    }
}

void AMFCodec::encodeAMF3String(std::vector<uint8_t>& buffer, const std::string& value) {
    // For simplicity, always encode as inline string (not using reference table)
    uint32_t length = static_cast<uint32_t>(value.size());
    encodeU29(buffer, (length << 1) | 0x01);  // Set inline flag
    buffer.insert(buffer.end(), value.begin(), value.end());
}

// =============================================================================
// Batch Operations
// =============================================================================

core::Result<std::vector<AMFValue>, AMFError> AMFCodec::decodeAll(
    AMFVersion version,
    const uint8_t* data,
    size_t length
) {
    std::vector<AMFValue> values;
    size_t offset = 0;

    while (offset < length) {
        size_t consumed = 0;

        auto result = (version == AMFVersion::AMF0)
            ? decodeAMF0Value(data + offset, length - offset, consumed, 0, offset)
            : decodeAMF3Value(data + offset, length - offset, consumed, 0, offset);

        if (result.isError()) {
            return core::Result<std::vector<AMFValue>, AMFError>::error(result.error());
        }

        values.push_back(std::move(result.value()));
        offset += consumed;
    }

    return core::Result<std::vector<AMFValue>, AMFError>::success(std::move(values));
}

} // namespace protocol
} // namespace openrtmp
