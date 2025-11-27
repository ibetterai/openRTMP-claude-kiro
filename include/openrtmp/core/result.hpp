// OpenRTMP - Cross-platform RTMP Server
// Result type for error handling without exceptions

#ifndef OPENRTMP_CORE_RESULT_HPP
#define OPENRTMP_CORE_RESULT_HPP

#include <variant>
#include <utility>
#include <stdexcept>
#include <type_traits>

namespace openrtmp {
namespace core {

/**
 * @brief A result type that represents either a success value or an error.
 *
 * This type is used throughout OpenRTMP for error handling without exceptions.
 * It follows the "Result" pattern common in modern C++ and Rust.
 *
 * @tparam T The success value type
 * @tparam E The error type
 */
template<typename T, typename E>
class Result {
public:
    /**
     * @brief Create a successful result with a value.
     * @param value The success value
     * @return Result containing the success value
     */
    static Result success(T value) {
        return Result(std::move(value), true);
    }

    /**
     * @brief Create an error result.
     * @param error The error value
     * @return Result containing the error
     */
    static Result error(E err) {
        return Result(std::move(err), false);
    }

    /**
     * @brief Check if the result represents success.
     * @return true if success, false if error
     */
    [[nodiscard]] bool isSuccess() const noexcept {
        return isSuccessFlag_;
    }

    /**
     * @brief Check if the result represents an error.
     * @return true if error, false if success
     */
    [[nodiscard]] bool isError() const noexcept {
        return !isSuccessFlag_;
    }

    /**
     * @brief Get the success value.
     * @return Reference to the success value
     * @throws std::logic_error if called on an error result
     */
    [[nodiscard]] T& value() & {
        if (!isSuccessFlag_) {
            throw std::logic_error("Attempted to access value on error result");
        }
        return std::get<T>(storage_);
    }

    /**
     * @brief Get the success value (const version).
     * @return Const reference to the success value
     * @throws std::logic_error if called on an error result
     */
    [[nodiscard]] const T& value() const& {
        if (!isSuccessFlag_) {
            throw std::logic_error("Attempted to access value on error result");
        }
        return std::get<T>(storage_);
    }

    /**
     * @brief Get the success value (rvalue version).
     * @return Rvalue reference to the success value
     * @throws std::logic_error if called on an error result
     */
    [[nodiscard]] T&& value() && {
        if (!isSuccessFlag_) {
            throw std::logic_error("Attempted to access value on error result");
        }
        return std::get<T>(std::move(storage_));
    }

    /**
     * @brief Get the error value.
     * @return Reference to the error value
     * @throws std::logic_error if called on a success result
     */
    [[nodiscard]] E& error() & {
        if (isSuccessFlag_) {
            throw std::logic_error("Attempted to access error on success result");
        }
        return std::get<E>(storage_);
    }

    /**
     * @brief Get the error value (const version).
     * @return Const reference to the error value
     * @throws std::logic_error if called on a success result
     */
    [[nodiscard]] const E& error() const& {
        if (isSuccessFlag_) {
            throw std::logic_error("Attempted to access error on success result");
        }
        return std::get<E>(storage_);
    }

    /**
     * @brief Get the success value or a default if error.
     * @param defaultValue The default value to return if error
     * @return The success value or the default
     */
    [[nodiscard]] T valueOr(T defaultValue) const& {
        if (isSuccessFlag_) {
            return std::get<T>(storage_);
        }
        return defaultValue;
    }

    /**
     * @brief Get the success value or a default if error (rvalue version).
     * @param defaultValue The default value to return if error
     * @return The success value or the default
     */
    [[nodiscard]] T valueOr(T defaultValue) && {
        if (isSuccessFlag_) {
            return std::get<T>(std::move(storage_));
        }
        return defaultValue;
    }

    // Default move operations
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    // Default copy operations
    Result(const Result&) = default;
    Result& operator=(const Result&) = default;

private:
    // Private constructor for internal use
    Result(T value, bool /* isSuccess */)
        : storage_(std::move(value)), isSuccessFlag_(true) {}

    Result(E err, bool /* isSuccess */)
        : storage_(std::move(err)), isSuccessFlag_(false) {}

    std::variant<T, E> storage_;
    bool isSuccessFlag_;
};

/**
 * @brief Specialization of Result for void success type.
 *
 * Used when an operation can succeed without returning a value,
 * but may still produce an error.
 *
 * @tparam E The error type
 */
template<typename E>
class Result<void, E> {
public:
    /**
     * @brief Create a successful void result.
     * @return Successful Result
     */
    static Result success() {
        return Result(true);
    }

    /**
     * @brief Create an error result.
     * @param error The error value
     * @return Result containing the error
     */
    static Result error(E err) {
        return Result(std::move(err));
    }

    /**
     * @brief Check if the result represents success.
     * @return true if success, false if error
     */
    [[nodiscard]] bool isSuccess() const noexcept {
        return isSuccessFlag_;
    }

    /**
     * @brief Check if the result represents an error.
     * @return true if error, false if success
     */
    [[nodiscard]] bool isError() const noexcept {
        return !isSuccessFlag_;
    }

    /**
     * @brief Get the error value.
     * @return Reference to the error value
     * @throws std::logic_error if called on a success result
     */
    [[nodiscard]] E& error() & {
        if (isSuccessFlag_) {
            throw std::logic_error("Attempted to access error on success result");
        }
        return error_;
    }

    /**
     * @brief Get the error value (const version).
     * @return Const reference to the error value
     * @throws std::logic_error if called on a success result
     */
    [[nodiscard]] const E& error() const& {
        if (isSuccessFlag_) {
            throw std::logic_error("Attempted to access error on success result");
        }
        return error_;
    }

    // Default move operations
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    // Default copy operations
    Result(const Result&) = default;
    Result& operator=(const Result&) = default;

private:
    // Private constructor for success
    explicit Result(bool /* isSuccess */)
        : error_{}, isSuccessFlag_(true) {}

    // Private constructor for error
    explicit Result(E err)
        : error_(std::move(err)), isSuccessFlag_(false) {}

    E error_;
    bool isSuccessFlag_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_RESULT_HPP
