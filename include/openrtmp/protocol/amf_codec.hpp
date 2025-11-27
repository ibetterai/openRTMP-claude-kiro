// OpenRTMP - Cross-platform RTMP Server
// AMF (Action Message Format) Codec - Encodes/decodes AMF0 and AMF3 data
//
// Implements AMF encoding and decoding per Adobe specifications:
// - AMF0: Original format used in RTMP command messages
// - AMF3: Enhanced format with more efficient encoding
//
// AMF0 Type Markers:
// - 0x00: Number (IEEE 754 double)
// - 0x01: Boolean
// - 0x02: String (16-bit length prefix)
// - 0x03: Object (key-value pairs)
// - 0x05: Null
// - 0x06: Undefined
// - 0x07: Reference (index to previously serialized object)
// - 0x08: ECMA Array (associative array with count hint)
// - 0x09: Object End marker
// - 0x0A: Strict Array (indexed array)
// - 0x0B: Date (milliseconds since epoch)
// - 0x0C: Long String (32-bit length prefix)
//
// Requirements coverage:
// - Requirements 3.1-3.7: RTMP command processing depends on AMF codec

#ifndef OPENRTMP_PROTOCOL_AMF_CODEC_HPP
#define OPENRTMP_PROTOCOL_AMF_CODEC_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <string>
#include <variant>
#include <chrono>
#include <optional>
#include <memory>

#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace protocol {

// =============================================================================
// AMF Type Markers
// =============================================================================

namespace amf0 {
    constexpr uint8_t NUMBER_MARKER = 0x00;
    constexpr uint8_t BOOLEAN_MARKER = 0x01;
    constexpr uint8_t STRING_MARKER = 0x02;
    constexpr uint8_t OBJECT_MARKER = 0x03;
    constexpr uint8_t MOVIECLIP_MARKER = 0x04;  // Reserved
    constexpr uint8_t NULL_MARKER = 0x05;
    constexpr uint8_t UNDEFINED_MARKER = 0x06;
    constexpr uint8_t REFERENCE_MARKER = 0x07;
    constexpr uint8_t ECMA_ARRAY_MARKER = 0x08;
    constexpr uint8_t OBJECT_END_MARKER = 0x09;
    constexpr uint8_t STRICT_ARRAY_MARKER = 0x0A;
    constexpr uint8_t DATE_MARKER = 0x0B;
    constexpr uint8_t LONG_STRING_MARKER = 0x0C;
    constexpr uint8_t UNSUPPORTED_MARKER = 0x0D;
    constexpr uint8_t RECORDSET_MARKER = 0x0E;  // Reserved
    constexpr uint8_t XML_DOC_MARKER = 0x0F;
    constexpr uint8_t TYPED_OBJECT_MARKER = 0x10;
    constexpr uint8_t AVMPLUS_MARKER = 0x11;    // Switch to AMF3
}

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
// Safety Limits
// =============================================================================

namespace amf_limits {
    constexpr size_t MAX_NESTING_DEPTH = 32;       ///< Maximum object nesting levels
    constexpr size_t MAX_STRING_LENGTH = 65536;    ///< Maximum string length (64KB)
    constexpr size_t MAX_LONG_STRING_LENGTH = 0xFFFFFFFF; ///< Maximum long string length
    constexpr int32_t AMF3_INT_MAX = 0x0FFFFFFF;   ///< Maximum AMF3 integer value (29 bits)
    constexpr int32_t AMF3_INT_MIN = -0x10000000;  ///< Minimum AMF3 integer value
}

// =============================================================================
// AMF Version Enum
// =============================================================================

/**
 * @brief AMF encoding version.
 */
enum class AMFVersion {
    AMF0,  ///< Original AMF format
    AMF3   ///< Enhanced AMF format
};

// =============================================================================
// AMF Error Type
// =============================================================================

/**
 * @brief Error information for AMF codec operations.
 */
struct AMFError {
    /**
     * @brief Error codes for AMF operations.
     */
    enum class Code {
        UnexpectedEnd,      ///< Truncated data - more bytes expected
        InvalidType,        ///< Unknown or unsupported type marker
        InvalidReference,   ///< Reference index out of bounds
        StringTooLong,      ///< String exceeds maximum allowed length
        NestingTooDeep,     ///< Object nesting exceeds maximum depth
        MalformedUTF8,      ///< Invalid UTF-8 encoding in string
        MalformedData,      ///< Generic malformed data error
        EncodingFailed      ///< Failed to encode value
    };

    Code code;              ///< Error code
    size_t offset;          ///< Byte offset where error occurred
    std::string message;    ///< Human-readable error message

    AMFError(Code c = Code::MalformedData, size_t off = 0, std::string msg = "")
        : code(c), offset(off), message(std::move(msg)) {}
};

// =============================================================================
// Forward Declarations
// =============================================================================

struct AMFValue;

// =============================================================================
// AMF Value Type
// =============================================================================

/**
 * @brief Variant type representing any AMF value.
 *
 * Supports all AMF0 and AMF3 data types with convenient accessor methods.
 */
struct AMFValue {
    /**
     * @brief AMF value type enumeration.
     */
    enum class Type {
        Null,           ///< Null value
        Undefined,      ///< Undefined value
        Boolean,        ///< Boolean (true/false)
        Number,         ///< IEEE 754 double-precision float
        String,         ///< UTF-8 string
        Object,         ///< Key-value object
        Array,          ///< Indexed array (alias for StrictArray)
        Date,           ///< Date (milliseconds since epoch)
        ECMAArray,      ///< Associative array (AMF0 specific)
        StrictArray,    ///< Strict indexed array
        Reference,      ///< Reference to previously serialized object
        TypedObject,    ///< Typed object with class name
        XML,            ///< XML document
        ByteArray       ///< Byte array (AMF3 specific)
    };

    Type type = Type::Null;

    /**
     * @brief Variant storage for all possible AMF values.
     */
    std::variant<
        std::nullptr_t,                          // Null/Undefined
        bool,                                    // Boolean
        double,                                  // Number/Integer
        std::string,                             // String/XML/LongString
        std::map<std::string, AMFValue>,         // Object/ECMAArray
        std::vector<AMFValue>,                   // StrictArray/Array
        std::chrono::milliseconds,               // Date
        std::vector<uint8_t>,                    // ByteArray (AMF3)
        uint16_t                                 // Reference index
    > data = nullptr;

    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------

    AMFValue() = default;

    static AMFValue makeNull() {
        AMFValue v;
        v.type = Type::Null;
        v.data = nullptr;
        return v;
    }

    static AMFValue makeUndefined() {
        AMFValue v;
        v.type = Type::Undefined;
        v.data = nullptr;
        return v;
    }

    static AMFValue makeBoolean(bool value) {
        AMFValue v;
        v.type = Type::Boolean;
        v.data = value;
        return v;
    }

    static AMFValue makeNumber(double value) {
        AMFValue v;
        v.type = Type::Number;
        v.data = value;
        return v;
    }

    static AMFValue makeString(std::string value) {
        AMFValue v;
        v.type = Type::String;
        v.data = std::move(value);
        return v;
    }

    static AMFValue makeObject(std::map<std::string, AMFValue> value) {
        AMFValue v;
        v.type = Type::Object;
        v.data = std::move(value);
        return v;
    }

    static AMFValue makeArray(std::vector<AMFValue> value) {
        AMFValue v;
        v.type = Type::StrictArray;
        v.data = std::move(value);
        return v;
    }

    // -------------------------------------------------------------------------
    // Type Checkers
    // -------------------------------------------------------------------------

    [[nodiscard]] bool isNull() const {
        return type == Type::Null || type == Type::Undefined;
    }

    [[nodiscard]] bool isBoolean() const {
        return type == Type::Boolean;
    }

    [[nodiscard]] bool isNumber() const {
        return type == Type::Number;
    }

    [[nodiscard]] bool isString() const {
        return type == Type::String;
    }

    [[nodiscard]] bool isObject() const {
        return type == Type::Object || type == Type::ECMAArray;
    }

    [[nodiscard]] bool isArray() const {
        return type == Type::StrictArray || type == Type::Array;
    }

    // -------------------------------------------------------------------------
    // Value Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] bool asBool() const {
        if (type == Type::Boolean) {
            return std::get<bool>(data);
        }
        return false;
    }

    [[nodiscard]] double asNumber() const {
        if (type == Type::Number) {
            return std::get<double>(data);
        }
        return 0.0;
    }

    [[nodiscard]] const std::string& asString() const {
        static const std::string empty;
        if (type == Type::String || type == Type::XML) {
            return std::get<std::string>(data);
        }
        return empty;
    }

    [[nodiscard]] const std::map<std::string, AMFValue>& asObject() const {
        static const std::map<std::string, AMFValue> empty;
        if (type == Type::Object || type == Type::ECMAArray) {
            return std::get<std::map<std::string, AMFValue>>(data);
        }
        return empty;
    }

    [[nodiscard]] const std::vector<AMFValue>& asArray() const {
        static const std::vector<AMFValue> empty;
        if (type == Type::StrictArray || type == Type::Array) {
            return std::get<std::vector<AMFValue>>(data);
        }
        return empty;
    }

    [[nodiscard]] std::chrono::milliseconds asDate() const {
        if (type == Type::Date) {
            return std::get<std::chrono::milliseconds>(data);
        }
        return std::chrono::milliseconds{0};
    }

    [[nodiscard]] const std::vector<uint8_t>& asByteArray() const {
        static const std::vector<uint8_t> empty;
        if (type == Type::ByteArray) {
            return std::get<std::vector<uint8_t>>(data);
        }
        return empty;
    }

    [[nodiscard]] uint16_t asReference() const {
        if (type == Type::Reference) {
            return std::get<uint16_t>(data);
        }
        return 0;
    }
};

// =============================================================================
// AMF Codec Interface
// =============================================================================

/**
 * @brief Interface for AMF encoding and decoding operations.
 *
 * This interface defines the contract for AMF codec implementations.
 */
class IAMFCodec {
public:
    virtual ~IAMFCodec() = default;

    // -------------------------------------------------------------------------
    // AMF0 Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Decode an AMF0 value from binary data.
     *
     * @param data Pointer to input data
     * @param length Length of input data
     * @param[out] bytesConsumed Number of bytes consumed during decoding
     * @return Decoded AMFValue or AMFError on failure
     */
    virtual core::Result<AMFValue, AMFError> decodeAMF0(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed
    ) = 0;

    /**
     * @brief Encode an AMF value to AMF0 binary format.
     *
     * @param value The AMFValue to encode
     * @return Encoded binary data or AMFError on failure
     */
    virtual core::Result<std::vector<uint8_t>, AMFError> encodeAMF0(
        const AMFValue& value
    ) = 0;

    // -------------------------------------------------------------------------
    // AMF3 Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Decode an AMF3 value from binary data.
     *
     * @param data Pointer to input data
     * @param length Length of input data
     * @param[out] bytesConsumed Number of bytes consumed during decoding
     * @return Decoded AMFValue or AMFError on failure
     */
    virtual core::Result<AMFValue, AMFError> decodeAMF3(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed
    ) = 0;

    /**
     * @brief Encode an AMF value to AMF3 binary format.
     *
     * @param value The AMFValue to encode
     * @return Encoded binary data or AMFError on failure
     */
    virtual core::Result<std::vector<uint8_t>, AMFError> encodeAMF3(
        const AMFValue& value
    ) = 0;

    // -------------------------------------------------------------------------
    // Batch Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Decode all AMF values from binary data.
     *
     * Used for decoding command messages that contain multiple AMF values
     * (e.g., command name, transaction ID, command object, optional arguments).
     *
     * @param version AMF version to use for decoding
     * @param data Pointer to input data
     * @param length Length of input data
     * @return Vector of decoded AMFValues or AMFError on failure
     */
    virtual core::Result<std::vector<AMFValue>, AMFError> decodeAll(
        AMFVersion version,
        const uint8_t* data,
        size_t length
    ) = 0;

    // -------------------------------------------------------------------------
    // Reference Table Management
    // -------------------------------------------------------------------------

    /**
     * @brief Reset reference tables.
     *
     * Should be called between command messages to ensure references
     * don't leak across message boundaries.
     */
    virtual void resetReferenceTables() = 0;
};

// =============================================================================
// AMF Codec Implementation
// =============================================================================

/**
 * @brief AMF Codec implementation.
 *
 * Implements IAMFCodec interface for encoding and decoding AMF0/AMF3 data.
 *
 * Safety features:
 * - Maximum nesting depth of 32 levels to prevent stack overflow
 * - Maximum string length of 64KB for AMF0 strings
 * - Reference table bounds checking
 * - Error reporting with byte offset for debugging
 */
class AMFCodec : public IAMFCodec {
public:
    /**
     * @brief Construct a new AMFCodec.
     */
    AMFCodec();

    /**
     * @brief Destructor.
     */
    ~AMFCodec() override = default;

    // Non-copyable but movable
    AMFCodec(const AMFCodec&) = delete;
    AMFCodec& operator=(const AMFCodec&) = delete;
    AMFCodec(AMFCodec&&) noexcept = default;
    AMFCodec& operator=(AMFCodec&&) noexcept = default;

    // IAMFCodec interface
    core::Result<AMFValue, AMFError> decodeAMF0(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed
    ) override;

    core::Result<std::vector<uint8_t>, AMFError> encodeAMF0(
        const AMFValue& value
    ) override;

    core::Result<AMFValue, AMFError> decodeAMF3(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed
    ) override;

    core::Result<std::vector<uint8_t>, AMFError> encodeAMF3(
        const AMFValue& value
    ) override;

    core::Result<std::vector<AMFValue>, AMFError> decodeAll(
        AMFVersion version,
        const uint8_t* data,
        size_t length
    ) override;

    void resetReferenceTables() override;

private:
    // -------------------------------------------------------------------------
    // AMF0 Decode Helpers
    // -------------------------------------------------------------------------

    core::Result<AMFValue, AMFError> decodeAMF0Value(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t depth,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0Number(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0Boolean(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0String(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0LongString(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0Object(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t depth,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0ECMAArray(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t depth,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0StrictArray(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t depth,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0Date(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    core::Result<AMFValue, AMFError> decodeAMF0Reference(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    core::Result<std::string, AMFError> decodeAMF0ShortString(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    // -------------------------------------------------------------------------
    // AMF0 Encode Helpers
    // -------------------------------------------------------------------------

    void encodeAMF0Value(
        std::vector<uint8_t>& buffer,
        const AMFValue& value
    );

    void encodeAMF0Number(std::vector<uint8_t>& buffer, double value);
    void encodeAMF0Boolean(std::vector<uint8_t>& buffer, bool value);
    void encodeAMF0String(std::vector<uint8_t>& buffer, const std::string& value);
    void encodeAMF0Object(std::vector<uint8_t>& buffer, const std::map<std::string, AMFValue>& obj);
    void encodeAMF0Array(std::vector<uint8_t>& buffer, const std::vector<AMFValue>& arr);

    // -------------------------------------------------------------------------
    // AMF3 Decode Helpers
    // -------------------------------------------------------------------------

    core::Result<AMFValue, AMFError> decodeAMF3Value(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t depth,
        size_t baseOffset
    );

    core::Result<uint32_t, AMFError> decodeU29(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    core::Result<std::string, AMFError> decodeAMF3String(
        const uint8_t* data,
        size_t length,
        size_t& bytesConsumed,
        size_t baseOffset
    );

    // -------------------------------------------------------------------------
    // AMF3 Encode Helpers
    // -------------------------------------------------------------------------

    void encodeAMF3Value(
        std::vector<uint8_t>& buffer,
        const AMFValue& value
    );

    void encodeU29(std::vector<uint8_t>& buffer, uint32_t value);
    void encodeAMF3String(std::vector<uint8_t>& buffer, const std::string& value);

    // -------------------------------------------------------------------------
    // Utility Helpers
    // -------------------------------------------------------------------------

    static double readDoubleBE(const uint8_t* data);
    static void writeDoubleBE(std::vector<uint8_t>& buffer, double value);

    // -------------------------------------------------------------------------
    // Reference Tables
    // -------------------------------------------------------------------------

    std::vector<AMFValue> amf0ObjectReferences_;
    std::vector<std::string> amf3StringReferences_;
    std::vector<AMFValue> amf3ObjectReferences_;
};

} // namespace protocol
} // namespace openrtmp

#endif // OPENRTMP_PROTOCOL_AMF_CODEC_HPP
