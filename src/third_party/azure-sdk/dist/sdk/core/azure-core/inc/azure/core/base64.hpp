// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Utility functions to help convert between binary data and UTF-8 encoded text that is
 * represented in Base64.
 */

#pragma once

#include <algorithm>
#include <cstdint> // defines std::uint8_t
#include <stdexcept>
#include <stdint.h> // deprecated, defines uint8_t in global namespace. TODO: Remove when uint8_t in the global namespace is removed.
#include <string>
#include <vector>

namespace Azure { namespace Core {

  /**
   * @brief Used to convert one form of data  into another, for example encoding binary data into
   * Base64 encoded octets.
   *
   * @note Base64 encoded data is a subset of the ASCII encoding (characters 0-127). As such,
   * it can be considered a subset of UTF-8.
   */
  class Convert final {
  private:
    // This type currently only contains static methods and hence disallowing instance creation.
    /**
     * @brief An instance of `%Convert` class cannot be created.
     *
     */
    Convert() = default;

  public:
    /**
     * @brief Encodes a vector of binary data using Base64.
     *
     * @param data The input vector that contains binary data to be encoded.
     * @return The Base64 encoded contents of the vector.
     */
    static std::string Base64Encode(const std::vector<uint8_t>& data);

    /**
     * @brief Decodes a Base64 encoded data into a vector of binary data.
     *
     * @param text Base64 encoded data to be decoded.
     * @return The decoded binary data.
     */
    static std::vector<uint8_t> Base64Decode(const std::string& text);
  };

  namespace _internal {
    /**
     * @brief Used to convert one form of data  into another, for example encoding binary data into
     * Base64 encoded octets.
     *
     * @note Base64 encoded data is a subset of the ASCII encoding (characters 0-127). As such,
     * it can be considered a subset of UTF-8.
     */
    class Convert final {
    private:
      // This type currently only contains static methods and hence disallowing instance creation.
      /**
       * @brief An instance of `%Convert` class cannot be created.
       *
       */
      Convert() = default;

    public:
      /**
       * @brief Encodes a string using Base64 encoding.
       *
       * @param data The input string that contains data to be encoded.
       * @return The Base64 encoded contents of the string.
       */
      static std::string Base64Encode(const std::string& data);
    };

    /**
     * @brief Provides conversion methods for Base64URL.
     *
     */
    class Base64Url final {

    public:
      static std::string Base64UrlEncode(const std::vector<uint8_t>& data)
      {
        auto base64 = Azure::Core::Convert::Base64Encode(data);
        // update to base64url
        auto trail = base64.find('=');
        if (trail != std::string::npos)
        {
          base64 = base64.substr(0, trail);
        }
        std::replace(base64.begin(), base64.end(), '+', '-');
        std::replace(base64.begin(), base64.end(), '/', '_');
        return base64;
      }

      static std::vector<uint8_t> Base64UrlDecode(const std::string& text)
      {
        std::string base64url(text);
        // base64url to base64
        std::replace(base64url.begin(), base64url.end(), '-', '+');
        std::replace(base64url.begin(), base64url.end(), '_', '/');
        switch (base64url.size() % 4)
        {
          case 0:
            break;
          case 2:
            base64url.append("==");
            break;
          case 3:
            base64url.append("=");
            break;
          default:
            throw std::invalid_argument("Unexpected Base64URL encoding in the HTTP response.");
        }
        return Azure::Core::Convert::Base64Decode(base64url);
      }
    };
  } // namespace _internal

}} // namespace Azure::Core
