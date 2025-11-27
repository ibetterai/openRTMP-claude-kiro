// OpenRTMP - Cross-platform RTMP Server
// Tests for Result type and common types

#include <gtest/gtest.h>
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/error_codes.hpp"

namespace openrtmp {
namespace core {
namespace test {

// Test Result<T, E> with success value
TEST(ResultTest, SuccessValueConstruction) {
    Result<int, std::string> result = Result<int, std::string>::success(42);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(result.isError());
    EXPECT_EQ(result.value(), 42);
}

// Test Result<T, E> with error value
TEST(ResultTest, ErrorValueConstruction) {
    Result<int, std::string> result = Result<int, std::string>::error("Something went wrong");

    EXPECT_FALSE(result.isSuccess());
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error(), "Something went wrong");
}

// Test Result<void, E> specialization for success
TEST(ResultTest, VoidSuccessConstruction) {
    Result<void, std::string> result = Result<void, std::string>::success();

    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(result.isError());
}

// Test Result<void, E> specialization for error
TEST(ResultTest, VoidErrorConstruction) {
    Result<void, std::string> result = Result<void, std::string>::error("Error occurred");

    EXPECT_FALSE(result.isSuccess());
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error(), "Error occurred");
}

// Test Result with complex types
TEST(ResultTest, ComplexTypeConstruction) {
    struct Data {
        int x;
        std::string name;
    };

    Result<Data, int> result = Result<Data, int>::success(Data{10, "test"});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(result.value().x, 10);
    EXPECT_EQ(result.value().name, "test");
}

// Test Result move semantics
TEST(ResultTest, MoveSemantics) {
    Result<std::string, int> result = Result<std::string, int>::success("hello");
    Result<std::string, int> moved = std::move(result);

    EXPECT_TRUE(moved.isSuccess());
    EXPECT_EQ(moved.value(), "hello");
}

// Test valueOr for success case
TEST(ResultTest, ValueOrSuccess) {
    Result<int, std::string> result = Result<int, std::string>::success(42);

    EXPECT_EQ(result.valueOr(0), 42);
}

// Test valueOr for error case
TEST(ResultTest, ValueOrError) {
    Result<int, std::string> result = Result<int, std::string>::error("error");

    EXPECT_EQ(result.valueOr(0), 0);
}

// Test ErrorCode enumeration
TEST(ErrorCodeTest, CommonErrorCodes) {
    EXPECT_NE(static_cast<int>(ErrorCode::Success), static_cast<int>(ErrorCode::Unknown));
    EXPECT_NE(static_cast<int>(ErrorCode::InvalidArgument), static_cast<int>(ErrorCode::Timeout));
    EXPECT_NE(static_cast<int>(ErrorCode::NotInitialized), static_cast<int>(ErrorCode::AlreadyExists));
}

// Test Error struct construction
TEST(ErrorTest, Construction) {
    Error err{ErrorCode::InvalidArgument, "Invalid port number", "port"};

    EXPECT_EQ(err.code, ErrorCode::InvalidArgument);
    EXPECT_EQ(err.message, "Invalid port number");
    EXPECT_EQ(err.context, "port");
}

// Test Error with empty context
TEST(ErrorTest, EmptyContext) {
    Error err{ErrorCode::Unknown, "Unknown error"};

    EXPECT_EQ(err.code, ErrorCode::Unknown);
    EXPECT_EQ(err.message, "Unknown error");
    EXPECT_TRUE(err.context.empty());
}

} // namespace test
} // namespace core
} // namespace openrtmp
