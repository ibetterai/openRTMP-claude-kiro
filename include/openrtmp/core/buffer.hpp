// OpenRTMP - Cross-platform RTMP Server
// Buffer utilities and byte manipulation helpers

#ifndef OPENRTMP_CORE_BUFFER_HPP
#define OPENRTMP_CORE_BUFFER_HPP

#include <vector>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <stdexcept>

namespace openrtmp {
namespace core {

/**
 * @brief A simple buffer class for storing and manipulating byte data.
 *
 * This class wraps std::vector<uint8_t> and provides convenient methods
 * for working with binary data in the RTMP protocol.
 */
class Buffer {
public:
    /**
     * @brief Default constructor creates an empty buffer.
     */
    Buffer() = default;

    /**
     * @brief Construct a buffer with initial size.
     * @param size Initial size in bytes (zero-initialized)
     */
    explicit Buffer(size_t size) : data_(size, 0) {}

    /**
     * @brief Construct a buffer from raw data.
     * @param data Pointer to source data
     * @param size Size of source data in bytes
     */
    Buffer(const uint8_t* data, size_t size)
        : data_(data, data + size) {}

    /**
     * @brief Construct a buffer from initializer list.
     * @param init Initializer list of bytes
     */
    Buffer(std::initializer_list<uint8_t> init)
        : data_(init) {}

    /**
     * @brief Construct a buffer from a vector.
     * @param vec Vector to copy from
     */
    explicit Buffer(std::vector<uint8_t> vec)
        : data_(std::move(vec)) {}

    // Default copy and move operations
    Buffer(const Buffer&) = default;
    Buffer& operator=(const Buffer&) = default;
    Buffer(Buffer&&) noexcept = default;
    Buffer& operator=(Buffer&&) noexcept = default;

    /**
     * @brief Get the size of the buffer.
     * @return Size in bytes
     */
    [[nodiscard]] size_t size() const noexcept {
        return data_.size();
    }

    /**
     * @brief Check if the buffer is empty.
     * @return true if empty
     */
    [[nodiscard]] bool empty() const noexcept {
        return data_.empty();
    }

    /**
     * @brief Get the capacity of the buffer.
     * @return Capacity in bytes
     */
    [[nodiscard]] size_t capacity() const noexcept {
        return data_.capacity();
    }

    /**
     * @brief Get a pointer to the underlying data.
     * @return Pointer to data
     */
    [[nodiscard]] uint8_t* data() noexcept {
        return data_.data();
    }

    /**
     * @brief Get a const pointer to the underlying data.
     * @return Const pointer to data
     */
    [[nodiscard]] const uint8_t* data() const noexcept {
        return data_.data();
    }

    /**
     * @brief Access a byte by index.
     * @param index Byte index
     * @return Reference to the byte
     */
    uint8_t& operator[](size_t index) {
        return data_[index];
    }

    /**
     * @brief Access a byte by index (const version).
     * @param index Byte index
     * @return Const reference to the byte
     */
    const uint8_t& operator[](size_t index) const {
        return data_[index];
    }

    /**
     * @brief Append data to the buffer.
     * @param init Data to append
     */
    void append(std::initializer_list<uint8_t> init) {
        data_.insert(data_.end(), init.begin(), init.end());
    }

    /**
     * @brief Append data from raw pointer.
     * @param data Pointer to source data
     * @param size Size of source data
     */
    void append(const uint8_t* data, size_t size) {
        data_.insert(data_.end(), data, data + size);
    }

    /**
     * @brief Append data from another buffer.
     * @param other Buffer to append
     */
    void append(const Buffer& other) {
        data_.insert(data_.end(), other.data_.begin(), other.data_.end());
    }

    /**
     * @brief Clear the buffer.
     */
    void clear() noexcept {
        data_.clear();
    }

    /**
     * @brief Reserve capacity.
     * @param capacity New capacity
     */
    void reserve(size_t capacity) {
        data_.reserve(capacity);
    }

    /**
     * @brief Resize the buffer.
     * @param size New size
     */
    void resize(size_t size) {
        data_.resize(size);
    }

    /**
     * @brief Resize the buffer with a fill value.
     * @param size New size
     * @param value Fill value for new bytes
     */
    void resize(size_t size, uint8_t value) {
        data_.resize(size, value);
    }

    /**
     * @brief Get the underlying vector.
     * @return Reference to internal vector
     */
    [[nodiscard]] std::vector<uint8_t>& vector() noexcept {
        return data_;
    }

    /**
     * @brief Get the underlying vector (const version).
     * @return Const reference to internal vector
     */
    [[nodiscard]] const std::vector<uint8_t>& vector() const noexcept {
        return data_;
    }

    // Iterator support
    auto begin() noexcept { return data_.begin(); }
    auto end() noexcept { return data_.end(); }
    auto begin() const noexcept { return data_.begin(); }
    auto end() const noexcept { return data_.end(); }
    auto cbegin() const noexcept { return data_.cbegin(); }
    auto cend() const noexcept { return data_.cend(); }

private:
    std::vector<uint8_t> data_;
};

/**
 * @brief A reader for extracting data from a buffer with position tracking.
 *
 * This class provides methods for reading various data types from a buffer
 * in both big-endian and little-endian byte orders, which are essential
 * for parsing RTMP protocol data.
 */
class BufferReader {
public:
    /**
     * @brief Construct a reader for a buffer.
     * @param buffer The buffer to read from
     */
    explicit BufferReader(const Buffer& buffer)
        : data_(buffer.data())
        , size_(buffer.size())
        , position_(0) {}

    /**
     * @brief Construct a reader from raw data.
     * @param data Pointer to data
     * @param size Size of data
     */
    BufferReader(const uint8_t* data, size_t size)
        : data_(data)
        , size_(size)
        , position_(0) {}

    /**
     * @brief Read a single byte.
     * @return The byte value
     * @throws std::out_of_range if not enough data
     */
    uint8_t readUint8() {
        checkRemaining(1);
        return data_[position_++];
    }

    /**
     * @brief Read a 16-bit unsigned integer in big-endian order.
     * @return The value
     * @throws std::out_of_range if not enough data
     */
    uint16_t readUint16BE() {
        checkRemaining(2);
        uint16_t value = static_cast<uint16_t>(
            (static_cast<uint32_t>(data_[position_]) << 8) |
            static_cast<uint32_t>(data_[position_ + 1])
        );
        position_ += 2;
        return value;
    }

    /**
     * @brief Read a 24-bit unsigned integer in big-endian order.
     * @return The value
     * @throws std::out_of_range if not enough data
     */
    uint32_t readUint24BE() {
        checkRemaining(3);
        uint32_t value = (static_cast<uint32_t>(data_[position_]) << 16) |
                         (static_cast<uint32_t>(data_[position_ + 1]) << 8) |
                         static_cast<uint32_t>(data_[position_ + 2]);
        position_ += 3;
        return value;
    }

    /**
     * @brief Read a 32-bit unsigned integer in big-endian order.
     * @return The value
     * @throws std::out_of_range if not enough data
     */
    uint32_t readUint32BE() {
        checkRemaining(4);
        uint32_t value = (static_cast<uint32_t>(data_[position_]) << 24) |
                         (static_cast<uint32_t>(data_[position_ + 1]) << 16) |
                         (static_cast<uint32_t>(data_[position_ + 2]) << 8) |
                         static_cast<uint32_t>(data_[position_ + 3]);
        position_ += 4;
        return value;
    }

    /**
     * @brief Read a 32-bit unsigned integer in little-endian order.
     * @return The value
     * @throws std::out_of_range if not enough data
     */
    uint32_t readUint32LE() {
        checkRemaining(4);
        uint32_t value = static_cast<uint32_t>(data_[position_]) |
                         (static_cast<uint32_t>(data_[position_ + 1]) << 8) |
                         (static_cast<uint32_t>(data_[position_ + 2]) << 16) |
                         (static_cast<uint32_t>(data_[position_ + 3]) << 24);
        position_ += 4;
        return value;
    }

    /**
     * @brief Read a number of bytes.
     * @param count Number of bytes to read
     * @return Vector of bytes
     * @throws std::out_of_range if not enough data
     */
    std::vector<uint8_t> readBytes(size_t count) {
        checkRemaining(count);
        std::vector<uint8_t> result(data_ + position_, data_ + position_ + count);
        position_ += count;
        return result;
    }

    /**
     * @brief Skip a number of bytes.
     * @param count Number of bytes to skip
     * @throws std::out_of_range if not enough data
     */
    void skip(size_t count) {
        checkRemaining(count);
        position_ += count;
    }

    /**
     * @brief Get the current read position.
     * @return Current position
     */
    [[nodiscard]] size_t position() const noexcept {
        return position_;
    }

    /**
     * @brief Seek to a specific position.
     * @param pos New position
     * @throws std::out_of_range if position is past end
     */
    void seek(size_t pos) {
        if (pos > size_) {
            throw std::out_of_range("Seek position past end of buffer");
        }
        position_ = pos;
    }

    /**
     * @brief Get the number of remaining bytes.
     * @return Remaining bytes
     */
    [[nodiscard]] size_t remaining() const noexcept {
        return size_ - position_;
    }

    /**
     * @brief Check if there are at least n bytes remaining.
     * @param n Number of bytes
     * @return true if at least n bytes remain
     */
    [[nodiscard]] bool hasRemaining(size_t n) const noexcept {
        return remaining() >= n;
    }

    /**
     * @brief Get the total size of the buffer.
     * @return Total size
     */
    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

private:
    void checkRemaining(size_t n) const {
        if (remaining() < n) {
            throw std::out_of_range("Not enough data in buffer");
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t position_;
};

/**
 * @brief A writer for appending data to a buffer with type conversions.
 *
 * This class provides methods for writing various data types to a buffer
 * in both big-endian and little-endian byte orders.
 */
class BufferWriter {
public:
    /**
     * @brief Construct a writer for a buffer.
     * @param buffer The buffer to write to
     */
    explicit BufferWriter(Buffer& buffer)
        : buffer_(buffer) {}

    /**
     * @brief Write a single byte.
     * @param value The byte value
     */
    void writeUint8(uint8_t value) {
        buffer_.append({value});
    }

    /**
     * @brief Write a 16-bit unsigned integer in big-endian order.
     * @param value The value
     */
    void writeUint16BE(uint16_t value) {
        buffer_.append({
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        });
    }

    /**
     * @brief Write a 24-bit unsigned integer in big-endian order.
     * @param value The value
     */
    void writeUint24BE(uint32_t value) {
        buffer_.append({
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        });
    }

    /**
     * @brief Write a 32-bit unsigned integer in big-endian order.
     * @param value The value
     */
    void writeUint32BE(uint32_t value) {
        buffer_.append({
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        });
    }

    /**
     * @brief Write a 32-bit unsigned integer in little-endian order.
     * @param value The value
     */
    void writeUint32LE(uint32_t value) {
        buffer_.append({
            static_cast<uint8_t>(value & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 24) & 0xFF)
        });
    }

    /**
     * @brief Write bytes from an initializer list.
     * @param data Data to write
     */
    void writeBytes(std::initializer_list<uint8_t> data) {
        buffer_.append(data);
    }

    /**
     * @brief Write bytes from a raw pointer.
     * @param data Pointer to data
     * @param size Size of data
     */
    void writeBytes(const uint8_t* data, size_t size) {
        buffer_.append(data, size);
    }

    /**
     * @brief Write bytes from a vector.
     * @param data Vector of bytes
     */
    void writeBytes(const std::vector<uint8_t>& data) {
        buffer_.append(data.data(), data.size());
    }

    /**
     * @brief Get the current size of the buffer.
     * @return Current size
     */
    [[nodiscard]] size_t size() const noexcept {
        return buffer_.size();
    }

private:
    Buffer& buffer_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_BUFFER_HPP
