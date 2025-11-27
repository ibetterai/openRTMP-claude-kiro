// OpenRTMP - Cross-platform RTMP Server
// Tests for Buffer utilities and byte manipulation helpers

#include <gtest/gtest.h>
#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace core {
namespace test {

// Test Buffer default construction
TEST(BufferTest, DefaultConstruction) {
    Buffer buffer;

    EXPECT_EQ(buffer.size(), 0);
    EXPECT_TRUE(buffer.empty());
    EXPECT_GE(buffer.capacity(), 0);
}

// Test Buffer construction with initial size
TEST(BufferTest, SizedConstruction) {
    Buffer buffer(1024);

    EXPECT_EQ(buffer.size(), 1024);
    EXPECT_FALSE(buffer.empty());
    EXPECT_GE(buffer.capacity(), 1024);
}

// Test Buffer construction from data
TEST(BufferTest, DataConstruction) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    Buffer buffer(data.data(), data.size());

    EXPECT_EQ(buffer.size(), 4);
    EXPECT_EQ(buffer[0], 0x01);
    EXPECT_EQ(buffer[3], 0x04);
}

// Test Buffer append
TEST(BufferTest, Append) {
    Buffer buffer;
    buffer.append({0x01, 0x02});
    buffer.append({0x03, 0x04});

    EXPECT_EQ(buffer.size(), 4);
    EXPECT_EQ(buffer[0], 0x01);
    EXPECT_EQ(buffer[3], 0x04);
}

// Test Buffer clear
TEST(BufferTest, Clear) {
    Buffer buffer(100);
    buffer.clear();

    EXPECT_EQ(buffer.size(), 0);
    EXPECT_TRUE(buffer.empty());
}

// Test Buffer reserve
TEST(BufferTest, Reserve) {
    Buffer buffer;
    buffer.reserve(1024);

    EXPECT_EQ(buffer.size(), 0);
    EXPECT_GE(buffer.capacity(), 1024);
}

// Test Buffer resize
TEST(BufferTest, Resize) {
    Buffer buffer;
    buffer.resize(100);

    EXPECT_EQ(buffer.size(), 100);
}

// Test Buffer data access
TEST(BufferTest, DataAccess) {
    Buffer buffer({0xAA, 0xBB, 0xCC});

    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_EQ(buffer.data()[0], 0xAA);
    EXPECT_EQ(buffer.data()[2], 0xCC);
}

// Test BufferReader read uint8
TEST(BufferReaderTest, ReadUint8) {
    Buffer buffer({0x42, 0xFF});
    BufferReader reader(buffer);

    EXPECT_EQ(reader.readUint8(), 0x42);
    EXPECT_EQ(reader.readUint8(), 0xFF);
    EXPECT_EQ(reader.remaining(), 0);
}

// Test BufferReader read uint16 big endian
TEST(BufferReaderTest, ReadUint16BE) {
    Buffer buffer({0x12, 0x34});
    BufferReader reader(buffer);

    EXPECT_EQ(reader.readUint16BE(), 0x1234);
}

// Test BufferReader read uint24 big endian
TEST(BufferReaderTest, ReadUint24BE) {
    Buffer buffer({0x12, 0x34, 0x56});
    BufferReader reader(buffer);

    EXPECT_EQ(reader.readUint24BE(), 0x123456);
}

// Test BufferReader read uint32 big endian
TEST(BufferReaderTest, ReadUint32BE) {
    Buffer buffer({0x12, 0x34, 0x56, 0x78});
    BufferReader reader(buffer);

    EXPECT_EQ(reader.readUint32BE(), 0x12345678);
}

// Test BufferReader read uint32 little endian
TEST(BufferReaderTest, ReadUint32LE) {
    Buffer buffer({0x78, 0x56, 0x34, 0x12});
    BufferReader reader(buffer);

    EXPECT_EQ(reader.readUint32LE(), 0x12345678);
}

// Test BufferReader read bytes
TEST(BufferReaderTest, ReadBytes) {
    Buffer buffer({0x01, 0x02, 0x03, 0x04, 0x05});
    BufferReader reader(buffer);

    auto bytes = reader.readBytes(3);
    EXPECT_EQ(bytes.size(), 3);
    EXPECT_EQ(bytes[0], 0x01);
    EXPECT_EQ(bytes[2], 0x03);
    EXPECT_EQ(reader.remaining(), 2);
}

// Test BufferReader skip
TEST(BufferReaderTest, Skip) {
    Buffer buffer({0x01, 0x02, 0x03, 0x04});
    BufferReader reader(buffer);

    reader.skip(2);
    EXPECT_EQ(reader.readUint8(), 0x03);
}

// Test BufferReader position and seek
TEST(BufferReaderTest, PositionAndSeek) {
    Buffer buffer({0x01, 0x02, 0x03, 0x04});
    BufferReader reader(buffer);

    reader.skip(3);
    EXPECT_EQ(reader.position(), 3);

    reader.seek(1);
    EXPECT_EQ(reader.position(), 1);
    EXPECT_EQ(reader.readUint8(), 0x02);
}

// Test BufferReader hasRemaining
TEST(BufferReaderTest, HasRemaining) {
    Buffer buffer({0x01, 0x02});
    BufferReader reader(buffer);

    EXPECT_TRUE(reader.hasRemaining(2));
    EXPECT_FALSE(reader.hasRemaining(3));
    reader.skip(1);
    EXPECT_TRUE(reader.hasRemaining(1));
    EXPECT_FALSE(reader.hasRemaining(2));
}

// Test BufferWriter write uint8
TEST(BufferWriterTest, WriteUint8) {
    Buffer buffer;
    BufferWriter writer(buffer);

    writer.writeUint8(0x42);
    writer.writeUint8(0xFF);

    EXPECT_EQ(buffer.size(), 2);
    EXPECT_EQ(buffer[0], 0x42);
    EXPECT_EQ(buffer[1], 0xFF);
}

// Test BufferWriter write uint16 big endian
TEST(BufferWriterTest, WriteUint16BE) {
    Buffer buffer;
    BufferWriter writer(buffer);

    writer.writeUint16BE(0x1234);

    EXPECT_EQ(buffer.size(), 2);
    EXPECT_EQ(buffer[0], 0x12);
    EXPECT_EQ(buffer[1], 0x34);
}

// Test BufferWriter write uint24 big endian
TEST(BufferWriterTest, WriteUint24BE) {
    Buffer buffer;
    BufferWriter writer(buffer);

    writer.writeUint24BE(0x123456);

    EXPECT_EQ(buffer.size(), 3);
    EXPECT_EQ(buffer[0], 0x12);
    EXPECT_EQ(buffer[1], 0x34);
    EXPECT_EQ(buffer[2], 0x56);
}

// Test BufferWriter write uint32 big endian
TEST(BufferWriterTest, WriteUint32BE) {
    Buffer buffer;
    BufferWriter writer(buffer);

    writer.writeUint32BE(0x12345678);

    EXPECT_EQ(buffer.size(), 4);
    EXPECT_EQ(buffer[0], 0x12);
    EXPECT_EQ(buffer[1], 0x34);
    EXPECT_EQ(buffer[2], 0x56);
    EXPECT_EQ(buffer[3], 0x78);
}

// Test BufferWriter write uint32 little endian
TEST(BufferWriterTest, WriteUint32LE) {
    Buffer buffer;
    BufferWriter writer(buffer);

    writer.writeUint32LE(0x12345678);

    EXPECT_EQ(buffer.size(), 4);
    EXPECT_EQ(buffer[0], 0x78);
    EXPECT_EQ(buffer[1], 0x56);
    EXPECT_EQ(buffer[2], 0x34);
    EXPECT_EQ(buffer[3], 0x12);
}

// Test BufferWriter write bytes
TEST(BufferWriterTest, WriteBytes) {
    Buffer buffer;
    BufferWriter writer(buffer);

    writer.writeBytes({0x01, 0x02, 0x03});

    EXPECT_EQ(buffer.size(), 3);
    EXPECT_EQ(buffer[0], 0x01);
    EXPECT_EQ(buffer[2], 0x03);
}

// Test BufferWriter write from pointer
TEST(BufferWriterTest, WriteBytesFromPointer) {
    Buffer buffer;
    BufferWriter writer(buffer);

    uint8_t data[] = {0xAA, 0xBB, 0xCC};
    writer.writeBytes(data, 3);

    EXPECT_EQ(buffer.size(), 3);
    EXPECT_EQ(buffer[0], 0xAA);
    EXPECT_EQ(buffer[2], 0xCC);
}

// Test roundtrip: write then read
TEST(BufferRoundtripTest, WriteAndRead) {
    Buffer buffer;
    BufferWriter writer(buffer);

    writer.writeUint8(0x01);
    writer.writeUint16BE(0x0203);
    writer.writeUint24BE(0x040506);
    writer.writeUint32BE(0x0708090A);
    writer.writeUint32LE(0x0B0C0D0E);

    BufferReader reader(buffer);

    EXPECT_EQ(reader.readUint8(), 0x01);
    EXPECT_EQ(reader.readUint16BE(), 0x0203);
    EXPECT_EQ(reader.readUint24BE(), 0x040506);
    EXPECT_EQ(reader.readUint32BE(), 0x0708090A);
    EXPECT_EQ(reader.readUint32LE(), 0x0B0C0D0E);
    EXPECT_EQ(reader.remaining(), 0);
}

} // namespace test
} // namespace core
} // namespace openrtmp
