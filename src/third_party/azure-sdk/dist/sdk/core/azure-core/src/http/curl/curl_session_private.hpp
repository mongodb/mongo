// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief The curl session consumes a curl connection to perform a request with it and start
 * streaming the response.
 *
 * @remark The curl session is a body stream derived class.
 */

#pragma once

#include "azure/core/http/http.hpp"
#include "curl_connection_pool_private.hpp"
#include "curl_connection_private.hpp"

#include <memory>
#include <string>

#ifdef _azure_TESTING_BUILD
// Define the class name that reads from ConnectionPool private members
namespace Azure { namespace Core { namespace Test {
  class CurlConnectionPool_connectionPoolTest_Test;
  class SdkWithLibcurl_globalCleanUp_Test;
}}} // namespace Azure::Core::Test
#endif

namespace Azure { namespace Core { namespace Http {

  /**
   * @brief Stateful component that controls sending an HTTP Request with libcurl over the wire.
   *
   * @remark This component does not use the classic libcurl easy interface to send and receive
   * bytes from the network using callbacks. Instead, `CurlSession` supports working with the custom
   * HTTP protocol option from libcurl to manually upload and download bytes from the network socket
   * using curl_easy_send() and curl_easy_recv().
   *
   * @remarks This component is expected to be used by an HTTP Transporter to ensure that
   * transporter to be reusable in multiple pipelines while every call to network is unique.
   */
  class CurlSession final : public Azure::Core::IO::BodyStream {
#ifdef _azure_TESTING_BUILD
    // Give access to private to this tests class
    friend class Azure::Core::Test::CurlConnectionPool_connectionPoolTest_Test;
    friend class Azure::Core::Test::SdkWithLibcurl_globalCleanUp_Test;
#endif
  private:
    /**
     * @brief Read one expected byte and throw if it is different than the \p expected
     *
     */
    void ReadExpected(uint8_t expected, Context const& context);

    /**
     * @brief Read `\\r\\n` from internal buffer or from the wire.
     *
     * @remark throw if `\\r\\n` are not the next data read.
     */
    void ReadCRLF(Context const& context);

    /**
     * @brief This is used to set the current state of a session.
     *
     * @remark The session needs to know what's the state on it when an exception occurs so the
     * connection is not moved back to the connection pool. When a new request is going to be sent,
     * the session will be in `PERFORM` until the request has been uploaded and a response code is
     * received from the server. At that point the state will change to `STREAMING`.
     * If there is any error before changing the state, the connection need to be cleaned up.
     *
     */
    enum class SessionState
    {
      PERFORM,
      STREAMING
    };

    /*
     * Enum used by ResponseBufferParser to control the parsing internal state while building
     * the HTTP RawResponse
     *
     */
    enum class ResponseParserState
    {
      StatusLine,
      Headers,
      EndOfHeaders,
    };

    /**
     * @brief stateful component used to read and parse a buffer to construct a valid HTTP
     * RawResponse.
     *
     * @remark It uses an internal string as a buffer to accumulate a response token (version, code,
     * header, etc.) until the next delimiter is found. Then it uses this string to keep building
     * the HTTP RawResponse.
     *
     * @remark Only status line and headers are parsed and built. Body is ignored by this
     * component. A libcurl session will use this component to build and return the HTTP
     * RawResponse with a body stream to the pipeline.
     */
    class ResponseBufferParser final {
    private:
      /**
       * @brief Controls what the parser is expecting during the reading process
       *
       */
      ResponseParserState state = ResponseParserState::StatusLine;
      /**
       * @brief Unique ptr to a response. Parser will create an Initial-valid HTTP RawResponse and
       * then it will append headers to it. This response is moved to a different owner once
       * parsing is completed.
       *
       */
      std::unique_ptr<RawResponse> m_response;
      /**
       * @brief Indicates if parser has found the end of the headers and there is nothing left for
       * the HTTP RawResponse.
       *
       */
      bool m_parseCompleted = false;

      bool m_delimiterStartInPrevPosition = false;

      /**
       * @brief This buffer is used when the parsed buffer doesn't contain a completed token. The
       * content from the buffer will be appended to this buffer. Once that a delimiter is found,
       * the token for the HTTP RawResponse is taken from this internal sting if it contains data.
       *
       * @remark This buffer allows a libcurl session to use any size of buffer to read from a
       * socket while constructing an initial valid HTTP RawResponse. No matter if the response
       * from wire contains hundreds of headers, we can use only one fixed size buffer to parse it
       * all.
       *
       */
      std::string m_internalBuffer;

      /**
       * @brief This method is invoked by the Parsing process if the internal state is set to
       * status code. Function will get the status-line expected tokens until finding the end of
       * status line delimiter.
       *
       * @remark When the end of status line delimiter is found, this method will create the HTTP
       * RawResponse. The HTTP RawResponse is constructed by default with body type as Stream.
       *
       * @param buffer Points to a memory address with all or some part of a HTTP status line.
       * @param bufferSize Indicates the size of the buffer.
       * @return Returns the index of the last parsed position from buffer.
       */
      size_t BuildStatusCode(uint8_t const* const buffer, size_t const bufferSize);

      /**
       * @brief This method is invoked by the Parsing process if the internal state is set to
       * headers. Function will keep adding headers to the HTTP RawResponse created before while
       * parsing an status line.
       *
       * @param buffer Points to a memory address with all or some part of a HTTP header.
       * @param bufferSize Indicates the size of the buffer.
       * @return Returns the index of the last parsed position from buffer. When the returned
       * value is smaller than the body size, means there is part of the body response in the
       * buffer.
       */
      size_t BuildHeader(uint8_t const* const buffer, size_t const bufferSize);

    public:
      /**
       * @brief Construct a new RawResponse Buffer Parser object.
       *
       */
      ResponseBufferParser() {}

      /**
       * @brief Parses the content of a buffer to construct a valid HTTP RawResponse. This method
       * is expected to be called over and over until it returns 0, indicating there is nothing
       * more to parse to build the HTTP RawResponse.
       *
       * @param buffer points to a memory area that contains, all or some part of an HTTP
       * response.
       * @param bufferSize Indicates the size of the buffer.
       * @return Returns the index of the last parsed position. Returning a 0 means nothing was
       * parsed and it is likely that the HTTP RawResponse is completed. Returning the same value
       * as the buffer size means all buffer was parsed and the HTTP might be completed or not.
       * Returning a value smaller than the buffer size will likely indicate that the HTTP
       * RawResponse is completed and that the rest of the buffer contains part of the response
       * body.
       *
       */
      size_t Parse(uint8_t const* const buffer, size_t const bufferSize);

      /**
       * @brief Indicates when the parser has completed parsing and building the HTTP RawResponse.
       *
       * @return `true` if parsing is completed; otherwise, `false`.
       */
      bool IsParseCompleted() const { return m_parseCompleted; }

      /**
       * @brief Moves the internal response to a different owner.
       *
       * @return Will move the response only if parsing is completed and if the HTTP RawResponse
       * was not moved before.
       */
      std::unique_ptr<RawResponse> ExtractResponse()
      {
        if (m_parseCompleted && m_response != nullptr)
        {
          return std::move(m_response);
        }
        return nullptr; // parse is not completed or response has been moved already.
      }
    };

    /**
     * @brief The current state of the session.
     *
     * @remark The state of the session is used to determine if a connection can be moved back to
     * the connection pool or not. A connection can be re-used only when the session state is
     * `STREAMING` and the response has been read completely.
     *
     */
    SessionState m_sessionState = SessionState::PERFORM;

    std::unique_ptr<CurlNetworkConnection> m_connection;

    /**
     * @brief unique ptr for the HTTP RawResponse. The session is responsable for creating the
     * response once that an HTTP status line is received.
     *
     */
    std::unique_ptr<RawResponse> m_response;

    /**
     * @brief The HTTP Request for to be used by the session.
     *
     */
    Request& m_request;

    /**
     * @brief Control field to handle the case when part of HTTP response body was copied to the
     * inner buffer. When a libcurl stream tries to read part of the body, this field will help to
     * decide how much data to take from the inner buffer before pulling more data from network.
     *
     * @note The initial value is set to the size of the inner buffer as a sentinel that indicate
     * that the buffer has not data or all data has already taken from it.
     */
    size_t m_bodyStartInBuffer = _detail::DefaultLibcurlReaderSize;

    /**
     * @brief Control field to handle the number of bytes containing relevant data within the
     * internal buffer. This is because internal buffer can be set to be size N but after writing
     * from wire into it, it can be holding less then N bytes.
     *
     */
    size_t m_innerBufferSize = _detail::DefaultLibcurlReaderSize;

    bool m_isChunkedResponseType = false;

    /**
     * @brief This is a copy of the value of an HTTP response header `content-length`. The value
     * is received as string and parsed to size_t. This field avoid parsing the string header
     * every time from HTTP RawResponse.
     *
     * @remark This value is also used to avoid trying to read more data from network than what we
     * are expecting to.
     *
     */
    int64_t m_contentLength = 0;

    /**
     * @brief For chunked responses, this field knows the size of the current chuck size server
     * will be sending.
     *
     */
    size_t m_chunkSize = 0;

    size_t m_sessionTotalRead = 0;

    /**
     * @brief If True, the connection is going to be "upgraded" into a websocket connection, so
     * block moving the connection to the pool.
     */
    bool m_connectionUpgraded = false;

    /**
     * @brief Internal buffer from a session used to read bytes from a socket. This buffer is only
     * used while constructing an HTTP RawResponse without adding a body to it. Customers would
     * provide their own buffer to copy from socket when reading the HTTP body using streams.
     *
     */
    uint8_t m_readBuffer[_detail::DefaultLibcurlReaderSize]
        = {0}; // to work with libcurl custom read.

    /**
     * @brief Function used when working with Streams to manually write from the HTTP Request to
     * the wire.
     *
     * @param context A context to control the request lifetime.
     *
     * @return CURL_OK when response is sent successfully.
     */
    CURLcode SendRawHttp(Context const& context);

    /**
     * @brief Upload body.
     *
     * @param context A context to control the request lifetime.
     *
     * @return Curl code.
     */
    CURLcode UploadBody(Context const& context);

    /**
     * @brief This function is used after sending an HTTP request to the server to read the HTTP
     * RawResponse from wire until the end of headers only.
     *
     * @param context A context to control the request lifetime.
     * @param reuseInternalBuffer Indicates whether the internal buffer should be reused.
     *
     * @return Curl code. CURLE_OK if successful, otherwise CURLE_RECV_ERROR.
     */
    CURLcode ReadStatusLineAndHeadersFromRawResponse(
        Context const& context,
        bool reuseInternalBuffer = false);

    /**
     * @brief Reads from inner buffer or from Wire until chunkSize is parsed and converted to
     * unsigned long long
     *
     * @param context A context to control the request lifetime.
     */
    void ParseChunkSize(Context const& context);

    /**
     * @brief Last HTTP status code read.
     *
     * @remark The last status is initialized as a bad request just as a way to know that there's
     * not a good request performed by the session. The status will be updated as soon as the
     * session sent a request and it is used to decide if a connection can be re-used or not.
     */
    Http::HttpStatusCode m_lastStatusCode = Http::HttpStatusCode::BadRequest;

    /**
     * @brief Holds information on whether the connection can be kept alive, based on HTTP protocol
     * version and the "Connection" HTTP header.
     *
     */
    bool m_httpKeepAlive = false;

    /**
     * @brief check whether an end of file has been reached.
     * @return `true` if end of file has been reached; otherwise, `false`.
     */
    bool IsEOF()
    {
      auto eof = m_isChunkedResponseType
          ? m_chunkSize == 0
          : static_cast<size_t>(m_contentLength) == m_sessionTotalRead;

      // `IsEOF` is called before trying to move a connection back to the connection pool.
      // If the session state is `PERFORM` it means the request could not complete an upload
      // operation (might have throw while uploading).
      // Connection should not be moved back to the connection pool on this scenario.
      return eof && m_sessionState != SessionState::PERFORM;
    }

    /**
     * @brief All connections will request to keep the channel open to re-use the
     * connection.
     *
     * @remark This option can be disabled from the transport adapter options. When disabled, the
     * session won't return connections to the connection pool. Connection will be closed as soon as
     * the request is completed.
     *
     */
    bool m_keepAlive = true;

    Azure::Nullable<std::string> m_httpProxy;
    Azure::Nullable<std::string> m_httpProxyUser;
    Azure::Nullable<std::string> m_httpProxyPassword;

    /**
     * @brief Implement Azure::Core::IO::BodyStream::OnRead. Calling this function pulls data
     * from the wire.
     *
     * @param context A context to control the request lifetime.
     * @param buffer Buffer where data from wire is written to.
     * @param count The number of bytes to read from the network.
     * @return The actual number of bytes read from the network.
     */
    size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override;

    inline std::string GetHTTPMessagePreBody(Azure::Core::Http::Request const& request);
    inline std::string GetHeadersAsString(Azure::Core::Http::Request const& request);
    inline static void SetHeader(
        Azure::Core::Http::RawResponse& response,
        std::string const& header);

  public:
    /**
     * @brief Construct a new Curl Session object. Init internal libcurl handler.
     *
     * @param request reference to an HTTP Request.
     * @param connection A connection from the connection pool.
     * @param curlOptions Transport adapter options.
     */
    CurlSession(
        Request& request,
        std::unique_ptr<CurlNetworkConnection> connection,
        CurlTransportOptions curlOptions)
        : m_connection(std::move(connection)), m_request(request),
          m_keepAlive(curlOptions.HttpKeepAlive), m_httpProxy(curlOptions.Proxy),
          m_httpProxyUser(curlOptions.ProxyUsername), m_httpProxyPassword(curlOptions.ProxyPassword)
    {
    }

    ~CurlSession() override
    {
      // mark connection as reusable only if entire response was read
      // If not, connection can't be reused because next Read will start from what it is currently
      // in the wire.
      // By not moving the connection back to the pool, it gets destroyed calling the connection
      // destructor to clean libcurl handle and close the connection.
      // IsEOF will also handle a connection that fail to complete an upload request.
      if (IsEOF() && m_keepAlive && !m_connectionUpgraded)
      {
        _detail::CurlConnectionPool::g_curlConnectionPool.MoveConnectionBackToPool(
            std::move(m_connection), m_httpKeepAlive);
      }
    }

    /**
     * @brief Function will use the HTTP request received in constructor to perform a network call
     * based on the HTTP request configuration.
     *
     * @param context A context to control the request lifetime.
     * @return CURLE_OK when the network call is completed successfully.
     */
    CURLcode Perform(Context const& context);

    /**
     * @brief Moved the ownership of the HTTP RawResponse out of the session.
     *
     * @return the unique ptr to the HTTP RawResponse or null if the HTTP RawResponse is not yet
     * created or was moved before.
     */
    std::unique_ptr<Azure::Core::Http::RawResponse> ExtractResponse();

    /**
     * @brief Implement #Azure::Core::IO::BodyStream length.
     *
     * @return The size of the payload.
     */
    int64_t Length() const override { return m_contentLength; }

    /**
     * @brief Return the network connection if the server indicated that the connection is upgraded.
     *
     * @return The network connection, or null if the connection was not upgraded.
     */
    std::unique_ptr<CurlNetworkConnection> ExtractConnection();
  };

}}} // namespace Azure::Core::Http
