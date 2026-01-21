// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Uniform Resource Locator (URL).
 */

#pragma once

#include "azure/core/case_insensitive_containers.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace Azure { namespace Core {
  namespace _detail {
    inline std::string FormatEncodedUrlQueryParameters(
        std::map<std::string, std::string> const& encodedQueryParameters)
    {
      {
        std::string queryStr;
        if (!encodedQueryParameters.empty())
        {
          auto separator = '?';
          for (const auto& q : encodedQueryParameters)
          {
            queryStr += separator + q.first + '=' + q.second;
            separator = '&';
          }
        }

        return queryStr;
      }
    }
  } // namespace _detail

  /**
   * @brief Represents the location where a request will be performed.
   *
   * @details It can be parsed and initialized from a string that contains all URL components
   * (scheme, host, path, etc.). Authority is not currently supported.
   */
  class Url final {
  private:
    std::string m_scheme;
    std::string m_host;
    uint16_t m_port{0};
    std::string m_encodedPath;
    // query parameters are all encoded
    std::map<std::string, std::string> m_encodedQueryParameters;

    std::string GetUrlWithoutQuery(bool relative) const;

    /**
     * @brief Finds the first '?' symbol and parses everything after it as query parameters.
     * separated by '&'.
     *
     * @param encodedQueryParameters `std::string` containing one or more query parameters.
     */
    void AppendQueryParameters(const std::string& encodedQueryParameters);

  public:
    /**
     * @brief Decodes \p value by transforming all escaped characters to it's non-encoded value.
     *
     * @param value URL-encoded string.
     * @return `std::string` with non-URL encoded values.
     */
    static std::string Decode(const std::string& value);

    /**
     * @brief Encodes \p value by escaping characters to the form of %HH where HH are hex digits.
     *
     * @note \p doNotEncodeSymbols arg can be used to explicitly ask this function to skip
     * characters from encoding. For instance, using this `= -` input would prevent encoding `=`, `
     * ` and `-`.
     *
     * @param value Non URL-encoded string.
     * @param doNotEncodeSymbols A string consisting of characters that do not need to be encoded.
     * @return std::string
     */
    static std::string Encode(const std::string& value, const std::string& doNotEncodeSymbols = "");

    /**
     * @brief Constructs a new, empty URL object.
     *
     */
    Url() {}

    /**
     * @brief Constructs a URL from a URL-encoded string.
     *
     * @param encodedUrl A URL-encoded string.
     * @note encodedUrl is expected to have all parts URL-encoded.
     */
    explicit Url(const std::string& encodedUrl);

    /************* Builder Url functions ****************/
    /******** API for building Url from scratch. Override state ********/

    /**
     * @brief Sets URL scheme.
     *
     * @param scheme URL scheme.
     */
    void SetScheme(const std::string& scheme) { m_scheme = scheme; }

    /**
     * @brief Sets URL host.
     *
     * @param encodedHost URL host, already encoded.
     */
    void SetHost(const std::string& encodedHost) { m_host = encodedHost; }

    /**
     * @brief Sets URL port.
     *
     * @param port URL port.
     */
    void SetPort(uint16_t port) { m_port = port; }

    /**
     * @brief Sets URL path.
     *
     * @param encodedPath URL path, already encoded.
     */
    void SetPath(const std::string& encodedPath) { m_encodedPath = encodedPath; }

    /**
     * @brief Sets the query parameters from an existing query parameter map.
     *
     * @note Keys and values in \p queryParameters are expected to be URL-encoded.
     *
     * @param queryParameters query parameters for request.
     */
    void SetQueryParameters(std::map<std::string, std::string> queryParameters)
    {
      // creates a copy and discard previous
      m_encodedQueryParameters = std::move(queryParameters);
    }

    // ===== APIs for mutating URL state: ======

    /**
     * @brief Appends an element of URL path.
     *
     * @param encodedPath URL path element to append, already encoded.
     */
    void AppendPath(const std::string& encodedPath)
    {
      if (!m_encodedPath.empty() && m_encodedPath.back() != '/')
      {
        m_encodedPath += '/';
      }
      m_encodedPath += encodedPath;
    }

    /**
     * @brief The value of a query parameter is expected to be non-URL-encoded and, by default, it
     * will be encoded before adding to the URL. Use \p isValueEncoded = true when the
     * value is already encoded.
     *
     * @note Overrides the value of existing query parameters.
     *
     * @param encodedKey Name of the query parameter, already encoded.
     * @param encodedValue Value of the query parameter, already encoded.
     */
    void AppendQueryParameter(const std::string& encodedKey, const std::string& encodedValue)
    {
      m_encodedQueryParameters[encodedKey] = encodedValue;
    }

    /**
     * @brief Removes an existing query parameter.
     *
     * @param encodedKey The name of the query parameter to be removed.
     */
    void RemoveQueryParameter(const std::string& encodedKey)
    {
      m_encodedQueryParameters.erase(encodedKey);
    }

    /************** API to read values from Url ***************/
    /**
     * @brief Gets URL host.
     *
     */
    const std::string& GetHost() const { return m_host; }

    /**
     * @brief Gets the URL path.
     *
     * @return const std::string&
     */
    const std::string& GetPath() const { return m_encodedPath; }

    /**
     * @brief Gets the port number set for the URL.
     *
     * @note If the port was not set for the URL, the returned port is 0. An HTTP request cannot
     * be performed to port zero, an HTTP client is expected to set the default port depending on
     * the request's schema when the port was not defined in the URL.
     *
     * @return The port number from the URL.
     */
    uint16_t GetPort() const { return m_port; }

    /**
     * @brief Gets a copy of the list of query parameters from the URL.
     *
     * @note The query parameters are URL-encoded.
     *
     * @return A copy of the query parameters map.
     */
    std::map<std::string, std::string> GetQueryParameters() const
    {
      return m_encodedQueryParameters;
    }

    /**
     * @brief Gets the URL scheme.
     *
     */
    const std::string& GetScheme() const { return m_scheme; }

    /**
     * @brief Gets the path and query parameters.
     *
     * @return Relative URL with URL-encoded query parameters.
     */
    std::string GetRelativeUrl() const;

    /**
     * @brief Gets Scheme, host, path and query parameters.
     *
     * @return Absolute URL with URL-encoded query parameters.
     */
    std::string GetAbsoluteUrl() const;
  };
}} // namespace Azure::Core
