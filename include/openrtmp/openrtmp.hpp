// OpenRTMP - Cross-platform RTMP Server
// Main header file

#ifndef OPENRTMP_OPENRTMP_HPP
#define OPENRTMP_OPENRTMP_HPP

/**
 * @file openrtmp.hpp
 * @brief Main header file for OpenRTMP library
 *
 * OpenRTMP is a cross-platform RTMP server library that enables live video
 * stream ingestion from encoders and broadcasters, with real-time distribution
 * to connected subscribers.
 *
 * Supported platforms:
 * - macOS 12.0+ (Monterey and later)
 * - Windows 11 (x64 and ARM64)
 * - Linux (kernel 5.4+)
 * - iOS 15.0+
 * - iPadOS 15.0+
 * - Android API 26+ (Android 8.0)
 */

// Version information
#define OPENRTMP_VERSION_MAJOR 0
#define OPENRTMP_VERSION_MINOR 1
#define OPENRTMP_VERSION_PATCH 0
#define OPENRTMP_VERSION_STRING "0.1.0"

// Core types
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/error_codes.hpp"
#include "openrtmp/core/buffer.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {

/**
 * @brief Get the library version as a string.
 * @return Version string in format "major.minor.patch"
 */
inline const char* version() {
    return OPENRTMP_VERSION_STRING;
}

/**
 * @brief Get the major version number.
 * @return Major version
 */
inline int versionMajor() {
    return OPENRTMP_VERSION_MAJOR;
}

/**
 * @brief Get the minor version number.
 * @return Minor version
 */
inline int versionMinor() {
    return OPENRTMP_VERSION_MINOR;
}

/**
 * @brief Get the patch version number.
 * @return Patch version
 */
inline int versionPatch() {
    return OPENRTMP_VERSION_PATCH;
}

} // namespace openrtmp

#endif // OPENRTMP_OPENRTMP_HPP
