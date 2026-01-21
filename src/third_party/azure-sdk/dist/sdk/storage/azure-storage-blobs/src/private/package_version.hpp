// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Provides version information.
 */

#pragma once

#define AZURE_STORAGE_BLOBS_VERSION_MAJOR 12
#define AZURE_STORAGE_BLOBS_VERSION_MINOR 13
#define AZURE_STORAGE_BLOBS_VERSION_PATCH 0
#define AZURE_STORAGE_BLOBS_VERSION_PRERELEASE ""

#define AZURE_STORAGE_BLOBS_VERSION_ITOA_HELPER(i) #i
#define AZURE_STORAGE_BLOBS_VERSION_ITOA(i) AZURE_STORAGE_BLOBS_VERSION_ITOA_HELPER(i)

namespace Azure { namespace Storage { namespace Blobs { namespace _detail {
  /**
   * @brief Provides version information.
   */
  class PackageVersion final {
  public:
    /**
     * @brief Major numeric identifier.
     */
    static constexpr int32_t Major = AZURE_STORAGE_BLOBS_VERSION_MAJOR;

    /**
     * @brief Minor numeric identifier.
     */
    static constexpr int32_t Minor = AZURE_STORAGE_BLOBS_VERSION_MINOR;

    /**
     * @brief Patch numeric identifier.
     */
    static constexpr int32_t Patch = AZURE_STORAGE_BLOBS_VERSION_PATCH;

    /**
     * @brief Indicates whether the SDK is in a pre-release state.
     */
    static constexpr bool IsPreRelease
        = sizeof(AZURE_STORAGE_BLOBS_VERSION_PRERELEASE) != sizeof("");

    /**
     * @brief The version in string format used for telemetry following the `semver.org` standard
     * (https://semver.org).
     */
    static constexpr const char* ToString()
    {
      return IsPreRelease
          ? AZURE_STORAGE_BLOBS_VERSION_ITOA(AZURE_STORAGE_BLOBS_VERSION_MAJOR) "." AZURE_STORAGE_BLOBS_VERSION_ITOA(
              AZURE_STORAGE_BLOBS_VERSION_MINOR) "." AZURE_STORAGE_BLOBS_VERSION_ITOA(AZURE_STORAGE_BLOBS_VERSION_PATCH) "-" AZURE_STORAGE_BLOBS_VERSION_PRERELEASE
          : AZURE_STORAGE_BLOBS_VERSION_ITOA(AZURE_STORAGE_BLOBS_VERSION_MAJOR) "." AZURE_STORAGE_BLOBS_VERSION_ITOA(
              AZURE_STORAGE_BLOBS_VERSION_MINOR) "." AZURE_STORAGE_BLOBS_VERSION_ITOA(AZURE_STORAGE_BLOBS_VERSION_PATCH);
    }
  };
}}}} // namespace Azure::Storage::Blobs::_detail

#undef AZURE_STORAGE_BLOBS_VERSION_ITOA_HELPER
#undef AZURE_STORAGE_BLOBS_VERSION_ITOA

#undef AZURE_STORAGE_BLOBS_VERSION_MAJOR
#undef AZURE_STORAGE_BLOBS_VERSION_MINOR
#undef AZURE_STORAGE_BLOBS_VERSION_PATCH
#undef AZURE_STORAGE_BLOBS_VERSION_PRERELEASE
