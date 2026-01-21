// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Define the HTTP raw response.
 */

#pragma once

#include "azure/core/case_insensitive_containers.hpp"
#include "azure/core/http/http_status_code.hpp"
#include "azure/core/io/body_stream.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Core { namespace Http {
  /**
   * @brief After receiving and interpreting a request message, a server responds with an HTTP
   * response message.
   */
  class RawResponse final {

  private:
    int32_t m_majorVersion;
    int32_t m_minorVersion;
    HttpStatusCode m_statusCode;
    std::string m_reasonPhrase;
    CaseInsensitiveMap m_headers;

    std::unique_ptr<Azure::Core::IO::BodyStream> m_bodyStream;
    std::vector<uint8_t> m_body;

    explicit RawResponse(
        int32_t majorVersion,
        int32_t minorVersion,
        HttpStatusCode statusCode,
        std::string const& reasonPhrase,
        std::unique_ptr<Azure::Core::IO::BodyStream> BodyStream)
        : m_majorVersion(majorVersion), m_minorVersion(minorVersion), m_statusCode(statusCode),
          m_reasonPhrase(reasonPhrase), m_bodyStream(std::move(BodyStream))
    {
    }

  public:
    /**
     * @brief Constructs `%RawResponse`.
     *
     * @param majorVersion HTTP protocol version major number.
     * @param minorVersion HTTP protocol version minor number.
     * @param statusCode HTTP status code.
     * @param reasonPhrase HTTP reason phrase.
     */
    explicit RawResponse(
        int32_t majorVersion,
        int32_t minorVersion,
        HttpStatusCode statusCode,
        std::string const& reasonPhrase)
        : RawResponse(majorVersion, minorVersion, statusCode, reasonPhrase, nullptr)
    {
    }

    /**
     * @brief Constructs `%RawResponse` from another.
     *
     * @remark The body stream won't be copied.
     *
     * @param response A reference for copying the raw response.
     */
    RawResponse(RawResponse const& response)
        : RawResponse(
            response.m_majorVersion,
            response.m_minorVersion,
            response.m_statusCode,
            response.m_reasonPhrase)
    {
      AZURE_ASSERT(m_bodyStream == nullptr);
      // Copy body
      m_body = response.GetBody();
    }

    /**
     * @brief Constructs `%RawResponse` by moving in another instance.
     *
     * @param response Another `%RawResponse` to move in.
     *
     */
    RawResponse(RawResponse&& response) = default;

    /**
     * @brief `%RawResponse` cannot be assigned.
     *
     */
    RawResponse& operator=(RawResponse const&) = delete;

    /**
     * @brief `%RawResponse` cannot be moved into.
     *
     */
    RawResponse& operator=(RawResponse&&) = delete;

    /**
     * @brief Destructs `%RawResponse`.
     *
     */
    ~RawResponse() = default;

    // ===== Methods used to build HTTP response =====

    /**
     * @brief Set an HTTP header to the #Azure::Core::Http::RawResponse.
     *
     * @remark The \p name must contain valid header name characters (RFC 7230).
     *
     * @param name The name for the header to be set or added.
     * @param value The value for the header to be set or added.
     *
     * @throw if \p name contains invalid characters.
     */
    void SetHeader(std::string const& name, std::string const& value);

    /**
     * @brief Set #Azure::Core::IO::BodyStream for this HTTP response.
     *
     * @param stream #Azure::Core::IO::BodyStream.
     */
    void SetBodyStream(std::unique_ptr<Azure::Core::IO::BodyStream> stream);

    /**
     * @brief Set HTTP response body for this HTTP response.
     *
     * @param body HTTP response body bytes.
     */
    void SetBody(std::vector<uint8_t> body) { this->m_body = std::move(body); }

    // adding getters for version and stream body. Clang will complain on macOS if we have unused
    // fields in a class

    /**
     * @brief Get HTTP protocol major version.
     *
     */
    int32_t GetMajorVersion() const { return m_majorVersion; }

    /**
     * @brief Get HTTP protocol minor version.
     *
     */
    int32_t GetMinorVersion() const { return m_minorVersion; }

    /**
     * @brief Get HTTP status code of the HTTP response.
     *
     */
    HttpStatusCode GetStatusCode() const;

    /**
     * @brief Get HTTP reason phrase code of the HTTP response.
     *
     */
    std::string const& GetReasonPhrase() const;

    /**
     * @brief Get HTTP response headers.
     *
     */
    CaseInsensitiveMap const& GetHeaders() const;

    /**
     * @brief Get HTTP response body as #Azure::Core::IO::BodyStream.
     *
     */
    std::unique_ptr<Azure::Core::IO::BodyStream> ExtractBodyStream()
    {
      // If m_bodyStream was moved before. nullptr is returned
      return std::move(this->m_bodyStream);
    }

    /**
     * @brief Get HTTP response body as vector of bytes.
     *
     */
    std::vector<uint8_t>& GetBody() { return this->m_body; }

    /**
     * @brief Get HTTP response body as vector of bytes.
     *
     */
    std::vector<uint8_t> const& GetBody() const { return this->m_body; }
  };
}}} // namespace Azure::Core::Http
