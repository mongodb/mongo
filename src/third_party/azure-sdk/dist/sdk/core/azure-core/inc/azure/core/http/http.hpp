// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief HTTP request and response functionality.
 */

#pragma once

#include "azure/core/case_insensitive_containers.hpp"
#include "azure/core/dll_import_export.hpp"
#include "azure/core/exception.hpp"
#include "azure/core/http/http_status_code.hpp"
#include "azure/core/http/raw_response.hpp"
#include "azure/core/internal/contract.hpp"
#include "azure/core/io/body_stream.hpp"
#include "azure/core/nullable.hpp"
#include "azure/core/url.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_azure_TESTING_BUILD)
// Define the class used from tests to validate retry enabled
namespace Azure { namespace Core { namespace Test {
  class TestHttp_getters_Test;
  class TestHttp_query_parameter_Test;
  class TestHttp_RequestStartTry_Test;
  class TestURL_getters_Test;
  class TestURL_query_parameter_Test;
  class TransportAdapter_headWithStream_Test;
  class TransportAdapter_putWithStream_Test;
  class TransportAdapter_deleteRequestWithStream_Test;
  class TransportAdapter_patchWithStream_Test;
  class TransportAdapter_putWithStreamOnFail_Test;
  class TransportAdapter_SizePutFromFile_Test;
  class TransportAdapter_SizePutFromFileDefault_Test;
  class TransportAdapter_SizePutFromFileBiggerPage_Test;
}}} // namespace Azure::Core::Test
#endif

namespace Azure { namespace Core { namespace Http {

  /*********************  Exceptions  **********************/
  /**
   * @brief An error while sending the HTTP request with the transport adapter.
   */
  class TransportException final : public Azure::Core::RequestFailedException {
  public:
    /**
     * @brief Constructs `%TransportException` with a \p message string.
     *
     * @remark The transport policy will throw this error whenever the transport adapter fail to
     * perform a request.
     *
     * @param what The explanatory string.
     */
    explicit TransportException(std::string const& what) : Azure::Core::RequestFailedException(what)
    {
    }
  };

  /**
   * @brief The range of bytes within an HTTP resource.
   *
   * @note Starts at an `Offset` and ends at `Offset + Length - 1` inclusively.
   */
  struct HttpRange final
  {
    /**
     * @brief The starting point of the HTTP Range.
     *
     */
    int64_t Offset = 0;

    /**
     * @brief The size of the HTTP Range.
     *
     */
    Azure::Nullable<int64_t> Length;
  };

  /**
   * @brief The method to be performed on the resource identified by the Request.
   */
  class HttpMethod final {
  public:
    /**
     * @brief Constructs `%HttpMethod` from string.
     *
     * @note Won't check if \p value is a known HttpMethod defined as per any RFC.
     *
     * @param value A given string to represent the `%HttpMethod`.
     */
    explicit HttpMethod(std::string value) : m_value(std::move(value)) {}

    /**
     * @brief Compares two instances of `%HttpMethod` for equality.
     *
     * @param other Some `%HttpMethod` instance to compare with.
     * @return `true` if instances are equal; otherwise, `false`.
     */
    bool operator==(const HttpMethod& other) const { return m_value == other.m_value; }

    /**
     * @brief Compares two instances of `%HttpMethod` for equality.
     *
     * @param other Some `%HttpMethod` instance to compare with.
     * @return `false` if instances are equal; otherwise, `true`.
     */
    bool operator!=(const HttpMethod& other) const { return !(*this == other); }

    /**
     * @brief Returns the `%HttpMethod` represented as a string.
     */
    const std::string& ToString() const { return m_value; }

    /**
     * @brief The representation of a `GET` HTTP method based on [RFC 7231]
     * (https://datatracker.ietf.org/doc/html/rfc7231#section-4.3.1).
     */
    AZ_CORE_DLLEXPORT const static HttpMethod Get;

    /**
     * @brief The representation of a `HEAD` HTTP method based on [RFC 7231]
     * (https://datatracker.ietf.org/doc/html/rfc7231#section-4.3.2).
     */
    AZ_CORE_DLLEXPORT const static HttpMethod Head;

    /**
     * @brief The representation of a `POST` HTTP method based on [RFC 7231]
     * (https://datatracker.ietf.org/doc/html/rfc7231#section-4.3.3).
     */
    AZ_CORE_DLLEXPORT const static HttpMethod Post;

    /**
     * @brief The representation of a `PUT` HTTP method based on [RFC 7231]
     * (https://datatracker.ietf.org/doc/html/rfc7231#section-4.3.4).
     */
    AZ_CORE_DLLEXPORT const static HttpMethod Put;

    /**
     * @brief The representation of a `DELETE` HTTP method based on [RFC 7231]
     * (https://datatracker.ietf.org/doc/html/rfc7231#section-4.3.5).
     */
    AZ_CORE_DLLEXPORT const static HttpMethod Delete;

    /**
     * @brief The representation of a `PATCH` HTTP method based on [RFC 5789]
     * (https://datatracker.ietf.org/doc/html/rfc5789).
     */
    AZ_CORE_DLLEXPORT const static HttpMethod Patch;

    /**
     * @brief The representation of an `OPTIONS` HTTP method based on [RFC 2616]
     * (https://datatracker.ietf.org/doc/html/rfc2616).
     */
    AZ_CORE_DLLEXPORT const static HttpMethod Options;

  private:
    std::string m_value;
  }; // extensible enum HttpMethod

  namespace Policies { namespace _internal {
    class RetryPolicy;
  }} // namespace Policies::_internal

  /**
   * @brief A request message from a client to a server.
   *
   * @details Includes, within the first line of the message, the HttpMethod to be applied to the
   * resource, the URL of the resource, and the protocol version in use.
   */
  class Request final {
    friend class Azure::Core::Http::Policies::_internal::RetryPolicy;
#if defined(_azure_TESTING_BUILD)
    // make tests classes friends to validate set Retry
    friend class Azure::Core::Test::TestHttp_getters_Test;
    friend class Azure::Core::Test::TestHttp_query_parameter_Test;
    friend class Azure::Core::Test::TestHttp_RequestStartTry_Test;
    friend class Azure::Core::Test::TestURL_getters_Test;
    friend class Azure::Core::Test::TestURL_query_parameter_Test;
    // make tests classes friends to validate private Request ctor that takes both stream and bool
    friend class Azure::Core::Test::TransportAdapter_headWithStream_Test;
    friend class Azure::Core::Test::TransportAdapter_putWithStream_Test;
    friend class Azure::Core::Test::TransportAdapter_deleteRequestWithStream_Test;
    friend class Azure::Core::Test::TransportAdapter_patchWithStream_Test;
    friend class Azure::Core::Test::TransportAdapter_putWithStreamOnFail_Test;
    friend class Azure::Core::Test::TransportAdapter_SizePutFromFile_Test;
    friend class Azure::Core::Test::TransportAdapter_SizePutFromFileDefault_Test;
    friend class Azure::Core::Test::TransportAdapter_SizePutFromFileBiggerPage_Test;
#endif

  private:
    HttpMethod m_method;
    Url m_url;
    CaseInsensitiveMap m_headers;
    CaseInsensitiveMap m_retryHeaders;

    Azure::Core::IO::BodyStream* m_bodyStream;

    // flag to know where to insert header
    bool m_retryModeEnabled{false};
    bool m_shouldBufferResponse{true};

    // Expected to be called by a Retry policy to reset all headers set after this function was
    // previously called
    void StartTry();

  public:
    /**
     * @brief Construct an #Azure::Core::Http::Request.
     *
     * @param httpMethod HttpMethod.
     * @param url %Request URL.
     * @param bodyStream #Azure::Core::IO::BodyStream.
     * @param shouldBufferResponse A boolean value indicating whether the returned response should
     * be buffered or returned as a body stream instead.
     */
    explicit Request(
        HttpMethod httpMethod,
        Url url,
        Azure::Core::IO::BodyStream* bodyStream,
        bool shouldBufferResponse)
        : m_method(std::move(httpMethod)), m_url(std::move(url)), m_bodyStream(bodyStream),
          m_retryModeEnabled(false), m_shouldBufferResponse(shouldBufferResponse)
    {
      AZURE_ASSERT_MSG(bodyStream, "The bodyStream pointer cannot be null.");
    }

    /**
     * @brief Constructs a `%Request`.
     *
     * @param httpMethod HTTP method.
     * @param url %Request URL.
     * @param bodyStream #Azure::Core::IO::BodyStream.
     */
    explicit Request(HttpMethod httpMethod, Url url, Azure::Core::IO::BodyStream* bodyStream)
        : Request(httpMethod, std::move(url), bodyStream, true)
    {
    }

    /**
     * @brief Constructs a `%Request`.
     *
     * @param httpMethod HTTP method.
     * @param url %Request URL.
     * @param shouldBufferResponse A boolean value indicating whether the returned response should
     * be buffered or returned as a body stream instead.
     */
    explicit Request(HttpMethod httpMethod, Url url, bool shouldBufferResponse);

    /**
     * @brief Constructs a `%Request`.
     *
     * @param httpMethod HTTP method.
     * @param url %Request URL.
     */
    explicit Request(HttpMethod httpMethod, Url url);

    /**
     * @brief Set an HTTP header to the #Azure::Core::Http::Request.
     *
     * @remark If the header key does not exists, it is added.
     *
     *
     * @param name The name for the header to be set or added.
     * @param value The value for the header to be set or added.
     *
     * @throw if \p name is an invalid header key.
     */
    void SetHeader(std::string const& name, std::string const& value);

    /**
     * @brief Gets a specific HTTP header from an #Azure::Core::Http::Request.
     *
     * @param name The name for the header to be retrieved.
     * @return The desired header, or an empty nullable if it is not found..
     *
     * @throw if \p name is an invalid header key.
     */
    Azure::Nullable<std::string> GetHeader(std::string const& name);

    /**
     * @brief Remove an HTTP header.
     *
     * @param name HTTP header name.
     */
    void RemoveHeader(std::string const& name);

    // Methods used by transport layer (and logger) to send request
    /**
     * @brief Get HttpMethod.
     *
     */
    HttpMethod const& GetMethod() const;

    /**
     * @brief Get HTTP headers.
     *
     * @remark Note that this function return a COPY of the headers for this request.
     *
     */
    CaseInsensitiveMap GetHeaders() const;

    /**
     * @brief Get HTTP body as #Azure::Core::IO::BodyStream.
     *
     */
    Azure::Core::IO::BodyStream* GetBodyStream() { return this->m_bodyStream; }

    /**
     * @brief Get HTTP body as #Azure::Core::IO::BodyStream.
     *
     */
    Azure::Core::IO::BodyStream const* GetBodyStream() const { return this->m_bodyStream; }

    /**
     * @brief A value indicating whether the returned raw response for this request will be buffered
     * within a memory buffer or if it will be returned as a body stream instead.
     */
    bool ShouldBufferResponse() { return this->m_shouldBufferResponse; }

    /**
     * @brief Get URL.
     *
     */
    Url& GetUrl() { return this->m_url; }

    /**
     * @brief Get URL.
     *
     */
    Url const& GetUrl() const { return this->m_url; }
  };

  namespace _detail {
    struct RawResponseHelpers final
    {
      /**
       * @brief Insert a header into \p headers checking that \p headerName does not contain invalid
       * characters.
       *
       * @param headers The headers map where to insert header.
       * @param headerName The header name for the header to be inserted.
       * @param headerValue The header value for the header to be inserted.
       *
       * @throw if \p headerName is invalid.
       */
      static void InsertHeaderWithValidation(
          CaseInsensitiveMap& headers,
          std::string const& headerName,
          std::string const& headerValue);

      static void inline SetHeader(
          Azure::Core::Http::RawResponse& response,
          uint8_t const* const first,
          uint8_t const* const last)
      {
        // get name and value from header
        auto start = first;
        auto end = std::find(start, last, ':');

        if (end == last)
        {
          throw std::invalid_argument("Invalid header. No delimiter ':' found.");
        }

        // Always toLower() headers
        auto const headerName
            = Azure::Core::_internal::StringExtensions::ToLower(std::string(start, end));
        start = end + 1; // start value
        while (start < last && (*start == ' ' || *start == '\t'))
        {
          ++start;
        }

        end = std::find(start, last, '\r');
        auto headerValue = std::string(start, end); // remove \r

        response.SetHeader(headerName, headerValue);
      }
    };
  } // namespace _detail

  namespace _internal {

    struct HttpShared final
    {
      AZ_CORE_DLLEXPORT static char const ContentType[];
      AZ_CORE_DLLEXPORT static char const ApplicationJson[];
      AZ_CORE_DLLEXPORT static char const Accept[];
      AZ_CORE_DLLEXPORT static char const MsRequestId[];
      AZ_CORE_DLLEXPORT static char const MsClientRequestId[];

      static inline std::string GetHeaderOrEmptyString(
          Azure::Core::CaseInsensitiveMap const& headers,
          std::string const& headerName)
      {
        auto header = headers.find(headerName);
        if (header != headers.end())
        {
          return header->second; // second is the header value.
        }
        return {}; // empty string
      }
    };
  } // namespace _internal

}}} // namespace Azure::Core::Http
