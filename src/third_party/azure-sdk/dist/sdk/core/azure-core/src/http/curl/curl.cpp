// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// cspell:words OCSP crls

#include "azure/core/base64.hpp"
#include "azure/core/platform.hpp"

#if defined(AZ_PLATFORM_WINDOWS)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#endif

#include "azure/core/platform.hpp"

#if defined(AZ_PLATFORM_WINDOWS)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#endif

#include "azure/core/http/curl_transport.hpp"
#include "azure/core/http/http.hpp"
#include "azure/core/http/policies/policy.hpp"
#include "azure/core/http/transport.hpp"
#include "azure/core/internal/diagnostics/log.hpp"
#include "azure/core/internal/strings.hpp"

// Private include
#include "curl_connection_pool_private.hpp"
#include "curl_connection_private.hpp"
#include "curl_session_private.hpp"

#if defined(AZ_PLATFORM_POSIX)
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER < 0x30000000L
#define USE_OPENSSL_1
#else
#define USE_OPENSSL_3
#endif // OPENSSL_VERSION_NUMBER < 0x30000000L

#include <openssl/asn1t.h>
#include <openssl/err.h>
// For OpenSSL > 3.0, we can use the new API to get the certificate's OCSP URL.
#if defined(USE_OPENSSL_3)
#include <openssl/http.h>
#endif
#if defined(USE_OPENSSL_1)
#include <openssl/ocsp.h>
#endif // defined(USE_OPENSSL_1)
#include <openssl/safestack.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#endif // AZ_PLATFORM_POSIX

#if defined(AZ_PLATFORM_POSIX)
#include <poll.h> // for poll()

#include <sys/socket.h> // for socket shutdown
#elif defined(AZ_PLATFORM_WINDOWS)
#include <winsock2.h> // for WSAPoll();
#endif // AZ_PLATFORM_POSIX/AZ_PLATFORM_WINDOWS

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace {
std::string const LogMsgPrefix = "[CURL Transport Adapter]: ";

template <typename T>
#if defined(_MSC_VER)
#pragma warning(push)
// C26812: The enum type 'CURLoption' is un-scoped. Prefer 'enum class' over 'enum' (Enum.3)
#pragma warning(disable : 26812)
#endif
inline bool SetLibcurlOption(
    Azure::Core::_internal::UniqueHandle<CURL> const& handle,
    CURLoption option,
    T value,
    CURLcode* outError)
{
  *outError = curl_easy_setopt(handle.get(), option, value);
  return *outError == CURLE_OK;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

enum class PollSocketDirection
{
  Read = 1,
  Write = 2,
};

/**
 * @brief Use poll from OS to check if socket is ready to be read or written.
 *
 * @param socketFileDescriptor socket descriptor.
 * @param direction poll events for read or write socket.
 * @param timeout  return if polling for more than \p timeout
 * @param context The context while polling that can be use to cancel waiting for socket.
 *
 * @return int with negative 1 upon any error, 0 on timeout or greater than zero if events were
 * detected (socket ready to be written/read)
 */
int pollSocketUntilEventOrTimeout(
    Azure::Core::Context const& context,
    curl_socket_t socketFileDescriptor,
    PollSocketDirection direction,
    long timeout)
{
#if !defined(AZ_PLATFORM_WINDOWS) && !defined(AZ_PLATFORM_POSIX)
  // platform does not support Poll().
  throw TransportException("Error while sending request. Platform does not support Poll()");
#endif

  struct pollfd poller;
  poller.fd = socketFileDescriptor;

  // set direction
  if (direction == PollSocketDirection::Read)
  {
    poller.events = POLLIN;
  }
  else
  {
    poller.events = POLLOUT;
  }

  // Call poll with the poller struct. Poll can handle multiple file descriptors by making an
  // pollfd array and passing the size of it as the second arg. Since we are only passing one fd,
  // we use 1 as arg.

  // Cancelation is possible by calling poll() with small time intervals instead of using the
  // requested timeout. The polling interval is 1 second.
  static constexpr std::chrono::milliseconds pollInterval(1000); // 1 second
  int result = 0;
  auto now = std::chrono::steady_clock::now();
  auto deadline = now + std::chrono::milliseconds(timeout);
  while (now < deadline)
  {
    // Before doing any work, check to make sure that the context hasn't already been cancelled.
    context.ThrowIfCancelled();
    int pollTimeoutMs = static_cast<int>(
        (std::min)(
            pollInterval, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
            .count());
#if defined(AZ_PLATFORM_POSIX)
    result = poll(&poller, 1, pollTimeoutMs);
    if (result < 0 && EINTR == errno)
    {
      now = std::chrono::steady_clock::now();
      continue;
    }
#elif defined(AZ_PLATFORM_WINDOWS)
    result = WSAPoll(&poller, 1, pollTimeoutMs);
#endif
    if (result != 0)
    {
      return result;
    }
    now = std::chrono::steady_clock::now();
  }
  // result can be 0 (timeout), > 0 (socket ready), or < 0 (error)
  return result;
}

using Azure::Core::Diagnostics::Logger;
using Azure::Core::Diagnostics::_internal::Log;

#if defined(AZ_PLATFORM_WINDOWS)
// Windows needs this after every write to socket or performance would be reduced to 1/4 for
// uploading operation.
// https://github.com/Azure/azure-sdk-for-cpp/issues/644
void WinSocketSetBuffSize(curl_socket_t socket)
{
  ULONG ideal{};
  DWORD ideallen{};
  // WSAloctl would get the ideal size for the socket buffer.
  if (WSAIoctl(socket, SIO_IDEAL_SEND_BACKLOG_QUERY, 0, 0, &ideal, sizeof(ideal), &ideallen, 0, 0)
      == 0)
  {
    // if WSAloctl succeeded (returned 0), set the socket buffer size.
    // Specifies the total per-socket buffer space reserved for sends.
    // https://docs.microsoft.com/windows/win32/api/winsock/nf-winsock-setsockopt
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char*)&ideal, sizeof(ideal));
  }
}
#endif

static void CleanupThread()
{
  // NOTE: Avoid using Log::Write in here as it may fail on macOS,
  // see issue: https://github.com/Azure/azure-sdk-for-cpp/issues/3224
  // This method can wake up in de-attached mode after the application has been terminated.
  // If that happens, trying to use `Log` would cause `abort` as it was previously deallocated.
  using namespace Azure::Core::Http::_detail;
  for (;;)
  {
    // Won't continue until the ConnectionPoolMutex is released from MoveConnectionBackToPool
    std::unique_lock<std::mutex> lockForPoolCleaning(
        CurlConnectionPool::g_curlConnectionPool.ConnectionPoolMutex);

    // Wait for the default time OR to the signal from the conditional variable.
    // wait_for releases the mutex lock when it goes to sleep and it takes the lock again when it
    // wakes up (or it's cancelled).
    if (CurlConnectionPool::g_curlConnectionPool.ConditionalVariableForCleanThread.wait_for(
            lockForPoolCleaning,
            std::chrono::milliseconds(DefaultCleanerIntervalMilliseconds),
            []() {
              return CurlConnectionPool::g_curlConnectionPool.ConnectionPoolIndex.size() == 0;
            }))
    {
      // Cancelled by another thread or no connections on wakeup
      CurlConnectionPool::g_curlConnectionPool.IsCleanThreadRunning = false;
      break;
    }

    decltype(CurlConnectionPool::g_curlConnectionPool
                 .ConnectionPoolIndex)::mapped_type connectionsToBeCleaned;

    // loop the connection pool index - Note: lock is re-taken for the mutex
    // Notes: The size of each host-index is always expected to be greater than 0 because the
    // host-index is removed anytime it becomes empty.
    for (auto index = CurlConnectionPool::g_curlConnectionPool.ConnectionPoolIndex.begin();
         index != CurlConnectionPool::g_curlConnectionPool.ConnectionPoolIndex.end();)
    {
      // Each pool index behaves as a Last-in-First-out (connections are added to the pool with
      // push_front). The last connection moved to the pool will be the first to be re-used. Because
      // of this, the oldest connection in the pool can be found at the end of the list. Looping the
      // connection pool backwards until a connection that is not expired is found or until all
      // connections are removed.
      auto& connectionList = index->second;
      auto connectionIter = connectionList.end();
      while (connectionIter != connectionList.begin())
      {
        --connectionIter;
        if ((*connectionIter)->IsExpired())
        {
          // remove connection from the pool and update the connection to the next one
          // which is going to be list.end()
          connectionsToBeCleaned.emplace_back(std::move(*connectionIter));
          connectionIter = connectionList.erase(connectionIter);
        }
        else
        {
          break;
        }
      }

      if (connectionList.empty())
      {
        index = CurlConnectionPool::g_curlConnectionPool.ConnectionPoolIndex.erase(index);
      }
      else
      {
        ++index;
      }
    }

    lockForPoolCleaning.unlock();
    // Do actual connections release work here, without holding the mutex.
  }
}

std::string PemEncodeFromBase64(std::string const& base64, std::string const& pemType)
{
  std::stringstream rv;
  rv << "-----BEGIN " << pemType << "-----" << std::endl;
  std::string encodedValue(base64);

  // Insert crlf characters every 80 characters into the base64 encoded key to make it
  // prettier.
  size_t insertPos = 80;
  while (insertPos < encodedValue.length())
  {
    encodedValue.insert(insertPos, "\r\n");
    insertPos += 82; /* 80 characters plus the \r\n we just inserted */
  }

  rv << encodedValue << std::endl << "-----END " << pemType << "-----" << std::endl;
  return rv.str();
}

Azure::Core::Http::CurlTransportOptions CurlTransportOptionsFromTransportOptions(
    Azure::Core::Http::Policies::TransportOptions const& transportOptions)
{
  Azure::Core::Http::CurlTransportOptions curlOptions;
  curlOptions.Proxy = transportOptions.HttpProxy;
  if (transportOptions.ProxyUserName.HasValue())
  {
    curlOptions.ProxyUsername = transportOptions.ProxyUserName;
  }
  curlOptions.ProxyPassword = transportOptions.ProxyPassword;

  curlOptions.SslOptions.EnableCertificateRevocationListCheck
      = transportOptions.EnableCertificateRevocationListCheck;

#if LIBCURL_VERSION_NUM >= 0x074D00 // 7.77.0
  if (!transportOptions.ExpectedTlsRootCertificate.empty())
  {
    curlOptions.SslOptions.PemEncodedExpectedRootCertificates
        = PemEncodeFromBase64(transportOptions.ExpectedTlsRootCertificate, "CERTIFICATE");
  }
#endif
  curlOptions.SslVerifyPeer = !transportOptions.DisableTlsCertificateValidation;
  return curlOptions;
}

} // namespace

using Azure::Core::Context;
using Azure::Core::Http::CurlConnection;
using Azure::Core::Http::CurlNetworkConnection;
using Azure::Core::Http::CurlSession;
using Azure::Core::Http::CurlTransport;
using Azure::Core::Http::CurlTransportOptions;
using Azure::Core::Http::HttpStatusCode;
using Azure::Core::Http::RawResponse;
using Azure::Core::Http::Request;
using Azure::Core::Http::TransportException;
using Azure::Core::Http::_detail::CurlConnectionPool;

Azure::Core::Http::_detail::CurlConnectionPool
    Azure::Core::Http::_detail::CurlConnectionPool::g_curlConnectionPool;

CurlTransport::CurlTransport(Azure::Core::Http::Policies::TransportOptions const& options)
    : CurlTransport(CurlTransportOptionsFromTransportOptions(options))
{
}

std::unique_ptr<RawResponse> CurlTransport::Send(Request& request, Context const& context)
{
  // Create CurlSession to perform request
  Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Creating a new session.");

  auto session = std::make_unique<CurlSession>(
      request,
      CurlConnectionPool::g_curlConnectionPool.ExtractOrCreateCurlConnection(request, m_options),
      m_options);

  CURLcode performing;

  // Try to send the request. If we get CURLE_UNSUPPORTED_PROTOCOL/CURLE_SEND_ERROR back, it means
  // the connection is either closed or the socket is not usable any more. In that case, let the
  // session be destroyed and create a new session to get another connection from connection pool.
  // Prevent from trying forever by using DefaultMaxOpenNewConnectionIntentsAllowed.
  for (auto getConnectionOpenIntent = 0;
       getConnectionOpenIntent < _detail::DefaultMaxOpenNewConnectionIntentsAllowed;
       getConnectionOpenIntent++)
  {
    performing = session->Perform(context);
    if (performing != CURLE_UNSUPPORTED_PROTOCOL && performing != CURLE_SEND_ERROR
        && performing != CURLE_RECV_ERROR)
    {
      break;
    }
    // Let session be destroyed and request a new connection. If the number of
    // request for connection has reached `RequestPoolResetAfterConnectionFailed`, ask the pool to
    // clean (remove connections) and create a new one. This is because, keep getting connections
    // that fail to perform means a general network disconnection where all connections in the pool
    // won't be no longer valid.
    session = std::make_unique<CurlSession>(
        request,
        CurlConnectionPool::g_curlConnectionPool.ExtractOrCreateCurlConnection(
            request,
            m_options,
            getConnectionOpenIntent + 1 >= _detail::RequestPoolResetAfterConnectionFailed),
        m_options);
  }

  if (performing != CURLE_OK)
  {
    throw TransportException(
        "Error while sending request. " + std::string(curl_easy_strerror(performing)));
  }
  if (HasWebSocketSupport())
  {
    std::unique_ptr<CurlNetworkConnection> upgradedConnection(session->ExtractConnection());
    if (upgradedConnection)
    {
      OnUpgradedConnection(std::move(upgradedConnection));
    }
  }

  Log::Write(
      Logger::Level::Verbose,
      LogMsgPrefix + "Request completed. Moving response out of session and session to response.");

  // Move Response out of the session
  auto response = session->ExtractResponse();
  // Move the ownership of the CurlSession (bodyStream) to the response
  response->SetBodyStream(std::move(session));
  return response;
}

CURLcode CurlSession::Perform(Context const& context)
{
  // Set the session state
  m_sessionState = SessionState::PERFORM;

  // libcurl settings after connection is open (headers)
  {
    auto headers = this->m_request.GetHeaders();
    auto hostHeader = headers.find("Host");
    if (hostHeader == headers.end())
    {
      Log::Write(Logger::Level::Verbose, LogMsgPrefix + "No Host in request headers. Adding it");
      std::string hostName = this->m_request.GetUrl().GetHost();
      auto port = this->m_request.GetUrl().GetPort();
      if (port != 0)
      {
        hostName += ":" + std::to_string(port);
      }
      this->m_request.SetHeader("Host", hostName);
    }
    if (this->m_request.GetMethod() != HttpMethod::Get
        && this->m_request.GetMethod() != HttpMethod::Head
        && this->m_request.GetMethod() != HttpMethod::Delete
        && headers.find("content-length") == headers.end())
    {
      Log::Write(Logger::Level::Verbose, LogMsgPrefix + "No content-length in headers. Adding it");
      this->m_request.SetHeader(
          "content-length", std::to_string(this->m_request.GetBodyStream()->Length()));
    }
  }
  // If we are using an HTTP proxy, connecting to an HTTP resource and it has been configured with a
  // username and password, we want to set the proxy authentication header.
  if (m_httpProxy.HasValue() && m_request.GetUrl().GetScheme() == "http"
      && m_httpProxyUser.HasValue() && m_httpProxyPassword.HasValue())
  {
    Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Setting proxy authentication header");
    this->m_request.SetHeader(
        "Proxy-Authorization",
        "Basic "
            + Azure::Core::_internal::Convert::Base64Encode(
                m_httpProxyUser.Value() + ":" + m_httpProxyPassword.Value()));
  }

  // use expect:100 for PUT requests. Server will decide if it can take our request
  if (this->m_request.GetMethod() == HttpMethod::Put)
  {
    Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Using 100-continue for PUT request");
    this->m_request.SetHeader("expect", "100-continue");
  }

  // Send request. If the connection assigned to this curlSession is closed or the socket is
  // somehow lost, libcurl will return CURLE_UNSUPPORTED_PROTOCOL
  // (https://curl.haxx.se/libcurl/c/curl_easy_send.html). Return the error back.
  Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Send request without payload");

  auto result = SendRawHttp(context);
  if (result != CURLE_OK)
  {
    return result;
  }

  Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Parse server response");
  result = ReadStatusLineAndHeadersFromRawResponse(context);
  if (result != CURLE_OK)
  {
    return result;
  }

  // non-PUT request are ready to be stream at this point. Only PUT request would start an uploading
  // transfer where we want to maintain the `PERFORM` state.
  if (this->m_request.GetMethod() != HttpMethod::Put)
  {
    m_sessionState = SessionState::STREAMING;
    return result;
  }

  Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Check server response before upload starts");
  // Check server response from Expect:100-continue for PUT;
  // This help to prevent us from start uploading data when Server can't handle it
  if (this->m_lastStatusCode != HttpStatusCode::Continue)
  {
    Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Server rejected the upload request");
    m_sessionState = SessionState::STREAMING;
    return result; // Won't upload.
  }

  Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Upload payload");
  if (this->m_bodyStartInBuffer < this->m_innerBufferSize)
  {
    // If internal buffer has more data after the 100-continue means Server return an error.
    // We don't need to upload body, just parse the response from Server and return
    result = ReadStatusLineAndHeadersFromRawResponse(context, true);
    if (result != CURLE_OK)
    {
      return result;
    }
    m_sessionState = SessionState::STREAMING;
    return result;
  }

  // Start upload
  result = this->UploadBody(context);
  if (result != CURLE_OK)
  {
    m_sessionState = SessionState::STREAMING;
    return result; // will throw transport exception before trying to read
  }

  Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Upload completed. Parse server response");
  result = ReadStatusLineAndHeadersFromRawResponse(context);
  if (result != CURLE_OK)
  {
    return result;
  }
  // If no throw at this point, the request is ready to stream.
  // If any throw happened before this point, the state will remain as PERFORM.
  m_sessionState = SessionState::STREAMING;
  return result;
}

std::unique_ptr<CurlNetworkConnection> CurlSession::ExtractConnection()
{
  if (m_connectionUpgraded)
  {
    return std::move(m_connection);
  }
  else
  {
    return nullptr;
  }
}

// Creates an HTTP Response with specific bodyType
static std::unique_ptr<RawResponse> CreateHTTPResponse(
    uint8_t const* const begin,
    uint8_t const* const last)
{
  // set response code, HTTP version and reason phrase (i.e. HTTP/1.1 200 OK)
  auto start = begin + 5; // HTTP = 4, / = 1, moving to 5th place for version
  auto end = std::find(start, last, '.');
  auto majorVersion = std::stoi(std::string(start, end));

  start = end + 1; // start of minor version
  end = std::find(start, last, ' ');
  auto minorVersion = std::stoi(std::string(start, end));

  start = end + 1; // start of status code
  end = std::find(start, last, ' ');
  auto statusCode = std::stoi(std::string(start, end));

  start = end + 1; // start of reason phrase
  end = std::find(start, last, '\r');
  auto reasonPhrase = std::string(start, end); // remove \r

  // allocate the instance of response to heap with shared ptr
  // So this memory gets delegated outside CurlTransport as a shared_ptr so memory will be
  // eventually released
  return std::make_unique<RawResponse>(
      static_cast<uint16_t>(majorVersion),
      static_cast<uint16_t>(minorVersion),
      HttpStatusCode(statusCode),
      reasonPhrase);
}

// Creates an HTTP Response with specific bodyType
static std::unique_ptr<RawResponse> CreateHTTPResponse(std::string const& header)
{
  return CreateHTTPResponse(
      reinterpret_cast<const uint8_t*>(header.data()),
      reinterpret_cast<const uint8_t*>(header.data() + header.size()));
}

// Send buffer thru the wire
CURLcode CurlConnection::SendBuffer(
    uint8_t const* buffer,
    size_t bufferSize,
    Context const& context)
{
  // Once you've shutdown the connection, we can't send any more data (although we can continue to
  // receive).
  if (IsShutdown())
  {
    return CURLE_SEND_ERROR;
  }
  for (size_t sentBytesTotal = 0; sentBytesTotal < bufferSize;)
  {
    // check cancelation for each chunk of data.
    // Next loop is expected to be called at most 2 times:
    // The first time we call `curl_easy_send()`, if it return CURLE_AGAIN it would call
    // `pollSocketUntilEventOrTimeout` to wait for socket to be ready to write.
    // `pollSocketUntilEventOrTimeout` will then handle cancelation token.
    // If socket is not ready before the timeout, Exception is thrown.
    // When socket is ready, it calls curl_easy_send() again (second loop iteration). It is not
    // expected to return CURLE_AGAIN (since socket is ready), so, a chuck of data will be uploaded
    // and result will be CURLE_OK which breaks the loop. Also, getting other than CURLE_OK or
    // CURLE_AGAIN throws.
    context.ThrowIfCancelled();
    for (CURLcode sendResult = CURLE_AGAIN; sendResult == CURLE_AGAIN;)
    {
      size_t sentBytesPerRequest = 0;
      sendResult = curl_easy_send(
          m_handle.get(),
          buffer + sentBytesTotal,
          bufferSize - sentBytesTotal,
          &sentBytesPerRequest);

      switch (sendResult)
      {
        case CURLE_OK: {
          sentBytesTotal += sentBytesPerRequest;
          break;
        }
        case CURLE_AGAIN: {
          // start polling operation with 1 min timeout
          auto pollUntilSocketIsReady = pollSocketUntilEventOrTimeout(
              context, m_curlSocket, PollSocketDirection::Write, 60000L);

          if (pollUntilSocketIsReady == 0)
          {
            throw TransportException("Timeout waiting for socket to upload.");
          }
          else if (pollUntilSocketIsReady < 0)
          { // negative value, error while polling
            throw TransportException("Error while polling for socket ready write");
          }

          // Ready to continue download.
          break;
        }
        default: {
          return sendResult;
        }
      }
    }
  }
#if defined(AZ_PLATFORM_WINDOWS)
  WinSocketSetBuffSize(m_curlSocket);
#endif
  return CURLE_OK;
}

CURLcode CurlSession::UploadBody(Context const& context)
{
  // Send body UploadStreamPageSize at a time (libcurl default)
  // NOTE: if stream is on top a contiguous memory, we can avoid allocating this copying buffer
  auto streamBody = this->m_request.GetBodyStream();
  CURLcode sendResult = CURLE_OK;

  auto unique_buffer
      = std::make_unique<uint8_t[]>(static_cast<size_t>(_detail::DefaultUploadChunkSize));

  while (true)
  {
    size_t rawRequestLen
        = streamBody->Read(unique_buffer.get(), _detail::DefaultUploadChunkSize, context);
    if (rawRequestLen == 0)
    {
      break;
    }
    sendResult = m_connection->SendBuffer(unique_buffer.get(), rawRequestLen, context);
    if (sendResult != CURLE_OK)
    {
      return sendResult;
    }
  }
  return sendResult;
}

// custom sending to wire an HTTP request
CURLcode CurlSession::SendRawHttp(Context const& context)
{
  // something like GET /path HTTP1.0 \r\nheaders\r\n
  auto rawRequest = GetHTTPMessagePreBody(this->m_request);
  auto rawRequestLen = rawRequest.size();

  CURLcode sendResult = m_connection->SendBuffer(
      reinterpret_cast<uint8_t const*>(rawRequest.data()),
      static_cast<size_t>(rawRequestLen),
      context);

  if (sendResult != CURLE_OK || this->m_request.GetMethod() == HttpMethod::Put)
  {
    return sendResult;
  }

  return this->UploadBody(context);
}

void inline CurlSession::SetHeader(
    Azure::Core::Http::RawResponse& response,
    std::string const& header)
{
  return Azure::Core::Http::_detail::RawResponseHelpers::SetHeader(
      response,
      reinterpret_cast<uint8_t const*>(header.data()),
      reinterpret_cast<uint8_t const*>(header.data() + header.size()));
}

inline std::string CurlSession::GetHeadersAsString(Azure::Core::Http::Request const& request)
{
  std::string requestHeaderString;

  for (auto const& header : request.GetHeaders())
  {
    requestHeaderString += header.first; // string (key)
    requestHeaderString += ": ";
    requestHeaderString += header.second; // string's value
    requestHeaderString += "\r\n";
  }
  requestHeaderString += "\r\n";

  return requestHeaderString;
}

// Writes an HTTP request with RFC 7230 without the body (head line and headers)
// https://tools.ietf.org/html/rfc7230#section-3.1.1
inline std::string CurlSession::GetHTTPMessagePreBody(Azure::Core::Http::Request const& request)
{
  std::string httpRequest(request.GetMethod().ToString());
  std::string url;

  // If we're not using a proxy server, *or* the URL we're connecting uses HTTPS then
  // we want to send the relative URL (the URL without the host, scheme, port or authn).
  // if we ARE using a proxy server and the request is not encrypted, we want to send the full URL.
  if (!m_httpProxy.HasValue() || request.GetUrl().GetScheme() == "https")
  {
    url = "/" + request.GetUrl().GetRelativeUrl();
  }
  else
  {
    url = request.GetUrl().GetAbsoluteUrl();
  }
  // HTTP version hardcoded to 1.1
  httpRequest += " " + url + " HTTP/1.1\r\n";

  // headers
  httpRequest += GetHeadersAsString(request);

  return httpRequest;
}

void CurlSession::ParseChunkSize(Context const& context)
{
  // Use this string to construct the chunk size. This is because we could have an internal
  // buffer like [headers...\r\n123], where 123 is chunk size but we still need to pull more
  // data fro wire to get the full chunkSize. Next data could be just [\r\n] or [456\r\n]
  auto strChunkSize = std::string();

  // Move to after chunk size
  for (bool keepPolling = true; keepPolling;)
  {
    for (size_t index = this->m_bodyStartInBuffer, iteration = 0; index < this->m_innerBufferSize;
         index++, iteration++)
    {
      strChunkSize.append(reinterpret_cast<char*>(&this->m_readBuffer[index]), 1);
      if (iteration > 1 && this->m_readBuffer[index] == '\n')
      {
        // get chunk size. Chunk size comes in Hex value
        try
        {
          // Required cast for MSVC x86
          this->m_chunkSize = static_cast<size_t>(std::stoull(strChunkSize, nullptr, 16));
        }
        catch (std::invalid_argument const&)
        {
          // Server can return something like `\n\r\n` for a chunk of zero length data. This is
          // allowed by RFC. `stoull` will throw invalid_argument if there is not at least one hex
          // digit to be parsed. For those cases, we consider the response as zero-length.
          this->m_chunkSize = 0;
        }

        if (this->m_chunkSize == 0)
        { // Response with no content. end of chunk
          keepPolling = false;
          /*
           * The index represents the current position while reading.
           * When the chunkSize is 0, the index should have already read up to the next CRLF.
           * When reading again, we want to start reading from the next position, so we need to add
           * 1 to the index.
           */
          this->m_bodyStartInBuffer = index + 1;
          break;
        }

        if (index + 1 == this->m_innerBufferSize)
        {
          /*
           * index + 1 represents the next possition to Read. If that's equal to the inner buffer
           * size it means that there is no more data and we need to fetch more from network. And
           * whatever we fetch will be the start of the chunk data. The bodyStart is set to 0 to
           * indicate the the next read call should read from the inner buffer start.
           */
          this->m_innerBufferSize = m_connection->ReadFromSocket(
              this->m_readBuffer, _detail::DefaultLibcurlReaderSize, context);
          this->m_bodyStartInBuffer = 0;
        }
        else
        {
          /*
           * index + 1 represents the next position to Read. If that's NOT equal to the inner
           * buffer size, it means that there is chunk data in the inner buffer. So, we set the
           * start to the next position to read.
           */
          this->m_bodyStartInBuffer = index + 1;
        }

        keepPolling = false;
        break;
      }
    }
    if (keepPolling)
    { // Read all internal buffer and \n was not found, pull from wire
      this->m_innerBufferSize = m_connection->ReadFromSocket(
          this->m_readBuffer, _detail::DefaultLibcurlReaderSize, context);
      this->m_bodyStartInBuffer = 0;
    }
  }
  return;
}

// Read status line plus headers to create a response with no body
CURLcode CurlSession::ReadStatusLineAndHeadersFromRawResponse(
    Context const& context,
    bool reuseInternalBuffer)
{
  auto parser = ResponseBufferParser();
  auto bufferSize = size_t();

  // Keep reading until all headers were read
  while (!parser.IsParseCompleted())
  {
    size_t bytesParsed = 0;
    if (reuseInternalBuffer)
    {
      // parse from internal buffer. This means previous read from server got more than one
      // response. This happens when Server returns a 100-continue plus an error code
      bufferSize = this->m_innerBufferSize - this->m_bodyStartInBuffer;
      bytesParsed = parser.Parse(this->m_readBuffer + this->m_bodyStartInBuffer, bufferSize);
      // if parsing from internal buffer is not enough, do next read from wire
      reuseInternalBuffer = false;
      // reset body start
      this->m_bodyStartInBuffer = _detail::DefaultLibcurlReaderSize;
    }
    else
    {
      // Try to fill internal buffer from socket.
      // If response is smaller than buffer, we will get back the size of the response
      bufferSize = m_connection->ReadFromSocket(
          this->m_readBuffer, _detail::DefaultLibcurlReaderSize, context);
      if (bufferSize == 0)
      {
        // closed connection, prevent application from keep trying to pull more bytes from the wire
        Log::Write(Logger::Level::Error, "Failed to read from socket");
        return CURLE_RECV_ERROR;
      }
      // returns the number of bytes parsed up to the body Start
      bytesParsed = parser.Parse(this->m_readBuffer, bufferSize);
    }

    if (bytesParsed < bufferSize)
    {
      this->m_bodyStartInBuffer = bytesParsed; // Body Start
    }
  }

  this->m_response = parser.ExtractResponse();
  this->m_innerBufferSize = bufferSize;
  this->m_lastStatusCode = this->m_response->GetStatusCode();

  // The logic below comes from the expectation that Azure services, particularly Storage, may not
  // conform to HTTP standards when it comes to handling 100-continue requests, and not send
  // "Connection: close" when they should. We do not know for sure if this is true, but this logic
  // did exist for libcurl transport in earlier C++ SDK versions.
  //
  // The idea is the following: if status code is not 2xx, and request header contains "Expect:
  // 100-continue" and request body length is not zero, we don't reuse the connection.
  //
  // More detailed description of what might happen if we don't have this logic:
  // 1. Storage SDK sends a PUT request with a non-empty request body (which means Content-Length
  // request header is not 0, let's say it's 6) and Expect: 100-continue request header, but it
  // doesn't send the header unless server returns 100 Continue status code.
  // 2. Storage service returns 4xx status code and response headers, but it doesn't want to close
  // this connection, so there's no Connection: close in response headers.
  // 3. Now both client and server agree to continue using this connection. But they do not agree in
  // the current status of this connection.
  //    3.1. Client side thinks the previous request is finished because it has received a status
  //    code and response headers. It should send a new HTTP request if there's any.
  //    3.2. Server side thinks the previous request is not finished because it hasn't received the
  //    request body. I tend to think this is a bug of server-side.
  // 4. Client side sends a new request, for example,
  //    HEAD /whatever/path HTTP/1.1
  //    host: foo.bar.com
  //    ...
  // 5. Server side takes the first 6 bytes (HEAD /) of the send request and thinks this is the
  // request body of the first request and discard it.
  // 6. Server side keeps reading the remaining data on the wire and thinks the first part
  // (whatever/path) is an HTTP verb. It fails the request with 400 invalid verb.
  bool non2xxAfter100ContinueWithNonzeroContentLength = false;
  {
    auto responseHttpCodeInt
        = static_cast<std::underlying_type<Http::HttpStatusCode>::type>(m_lastStatusCode);
    if (responseHttpCodeInt < 200 || responseHttpCodeInt >= 300)
    {
      const auto requestExpectHeader = m_request.GetHeader("Expect");
      if (requestExpectHeader.HasValue())
      {
        const auto requestExpectHeaderValueLowercase
            = Core::_internal::StringExtensions::ToLower(requestExpectHeader.Value());
        if (requestExpectHeaderValueLowercase == "100-continue")
        {
          const auto requestContentLengthHeaderValue = m_request.GetHeader("Content-Length");
          if (requestContentLengthHeaderValue.HasValue()
              && requestContentLengthHeaderValue.Value() != "0")
          {
            non2xxAfter100ContinueWithNonzeroContentLength = true;
          }
        }
      }
    }
  }

  if (non2xxAfter100ContinueWithNonzeroContentLength)
  {
    m_httpKeepAlive = false;
  }
  else
  {
    // HTTP <=1.0 is "close" by default. HTTP 1.1 is "keep-alive" by default.
    // The value can also be "keep-alive, close" (i.e. "both are fine"), in which case we are
    // preferring to treat it as keep-alive.
    // (https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Connection)
    // Should it come to HTTP/2 and HTTP/3, they are "keep-alive", but any response from HTTP/2 or
    // /3 containing a "Connection" header should be considered malformed.
    // (HTTP/2: https://httpwg.org/specs/rfc9113.html#ConnectionSpecific
    //  HTTP/3: https://httpwg.org/specs/rfc9114.html#rfc.section.4.2)
    //
    // HTTP/2+ are supposed to create persistent ("keep-alive") connections per host,
    // and close them by inactivity timeout. Given that we don't have such mechanism implemented
    // at this moment, we are closing all non-1.x connections immediately, which, in the worst case,
    // would only mean there's a perf hit, but the communication flow is expected to be correct.
    if (m_response->GetMajorVersion() == 1)
    {
      std::string connectionHeaderValue;
      {
        const Core::CaseInsensitiveMap& responseHeaders = m_response->GetHeaders();
        const auto connectionHeader = responseHeaders.find("Connection");
        if (connectionHeader != responseHeaders.cend())
        {
          connectionHeaderValue
              = Core::_internal::StringExtensions::ToLower(connectionHeader->second);
        }
      }

      const bool hasConnectionKeepAlive
          = connectionHeaderValue.find("keep-alive") != std::string::npos;

      if (m_response->GetMinorVersion() >= 1)
      {
        // HTTP/1.1+
        const bool hasConnectionClose = connectionHeaderValue.find("close") != std::string::npos;

        m_httpKeepAlive = (!hasConnectionClose || hasConnectionKeepAlive);
      }
      else
      {
        // HTTP/1.0
        m_httpKeepAlive = hasConnectionKeepAlive;
      }
    }
    else
    {
      // We don't expect HTTP/0.9, 2.0 or 3.0 in responses.
      // Barring rejecting as malformed, the safest thing to do here is to assume the connection is
      // not reusable.
      m_httpKeepAlive = false;
    }
  }

  // For Head request, set the length of body response to 0.
  // Response will give us content-length as if we were not doing Head saying what would it be the
  // length of the body. However, Server won't send body
  // For NoContent status code, also need to set contentLength to 0.
  // https://github.com/Azure/azure-sdk-for-cpp/issues/406
  if (this->m_request.GetMethod() == HttpMethod::Head
      || this->m_lastStatusCode == HttpStatusCode::NoContent
      || this->m_lastStatusCode == HttpStatusCode::NotModified)
  {
    this->m_contentLength = 0;
    this->m_bodyStartInBuffer = _detail::DefaultLibcurlReaderSize;
    return CURLE_OK;
  }

  // headers are already lowerCase at this point
  auto const& headers = this->m_response->GetHeaders();

  // Check if server has return the connection header. This header can be used to stop re-using the
  // connection. The `Iot Edge Blob Storage Module` is known to return this after some time re-using
  // the same http secured channel.
  auto connectionHeader = headers.find("connection");
  if (connectionHeader != headers.end())
  {
    if (Azure::Core::_internal::StringExtensions::LocaleInvariantCaseInsensitiveEqual(
            connectionHeader->second, "close"))
    {
      // Use connection shut-down so it won't be moved it back to the connection pool.
      m_connection->Shutdown();
    }
    // If the server indicated that the connection header is "upgrade", it means that this
    // is a WebSocket connection so the caller may be upgrading the connection.
    if (Azure::Core::_internal::StringExtensions::LocaleInvariantCaseInsensitiveEqual(
            connectionHeader->second, "upgrade"))
    {
      m_connectionUpgraded = true;
    }
  }

  auto isContentLengthHeaderInResponse = headers.find("content-length");
  if (isContentLengthHeaderInResponse != headers.end())
  {
    this->m_contentLength
        = static_cast<int64_t>(std::stoull(isContentLengthHeaderInResponse->second.data()));
    return CURLE_OK;
  }

  // No content-length from headers, check transfer-encoding
  this->m_contentLength = -1;
  auto isTransferEncodingHeaderInResponse = headers.find("transfer-encoding");
  if (isTransferEncodingHeaderInResponse != headers.end())
  {
    auto& headerValue = isTransferEncodingHeaderInResponse->second;
    auto isChunked = headerValue.find("chunked");

    if (isChunked != std::string::npos)
    {
      // set curl session to know response is chunked
      // This will be used to remove chunked info while reading
      this->m_isChunkedResponseType = true;

      // Need to move body start after chunk size
      if (this->m_bodyStartInBuffer >= this->m_innerBufferSize)
      { // if nothing on inner buffer, pull from wire
        this->m_innerBufferSize = m_connection->ReadFromSocket(
            this->m_readBuffer, _detail::DefaultLibcurlReaderSize, context);
        if (this->m_innerBufferSize == 0)
        {
          // closed connection, prevent application from keep trying to pull more bytes from the
          // wire
          Log::Write(Logger::Level::Error, "Failed to read from socket");
          return CURLE_RECV_ERROR;
        }
        this->m_bodyStartInBuffer = 0;
      }

      ParseChunkSize(context);
      return CURLE_OK;
    }
  }
  /*
  https://tools.ietf.org/html/rfc7230#section-3.3.3
   7.  Otherwise, this is a response message without a declared message
       body length, so the message body length is determined by the
       number of octets received prior to the server closing the
       connection.
  */
  return CURLE_OK;
}

/**
 * @brief Reads data from network and validates the data is equal to \p expected.
 *
 * @param expected The data that should came from the wire.
 * @param context A context to control the request lifetime.
 */
void CurlSession::ReadExpected(uint8_t expected, Context const& context)
{
  if (this->m_bodyStartInBuffer >= this->m_innerBufferSize)
  {
    // end of buffer, pull data from wire
    this->m_innerBufferSize = m_connection->ReadFromSocket(
        this->m_readBuffer, _detail::DefaultLibcurlReaderSize, context);
    if (this->m_innerBufferSize == 0)
    {
      // closed connection, prevent application from keep trying to pull more bytes from the wire
      throw TransportException(
          "Connection was closed by the server while trying to read a response");
    }
    this->m_bodyStartInBuffer = 0;
  }
  auto data = this->m_readBuffer[this->m_bodyStartInBuffer];
  if (data != expected)
  {
    throw TransportException(
        "Unexpected format in HTTP response. Expecting: " + std::to_string(expected)
        + ", but found: " + std::to_string(data) + ".");
  }
  this->m_bodyStartInBuffer += 1;
}

void CurlSession::ReadCRLF(Context const& context)
{
  ReadExpected('\r', context);
  ReadExpected('\n', context);
}

// Read from curl session
size_t CurlSession::OnRead(uint8_t* buffer, size_t count, Context const& context)
{
  if (count == 0 || this->IsEOF())
  {
    return 0;
  }

  // check if all chunked is all read already
  if (this->m_isChunkedResponseType && this->m_chunkSize == this->m_sessionTotalRead)
  {
    ReadCRLF(context);
    // Reset session read counter for next chunk
    this->m_sessionTotalRead = 0;
    // get the size of next chunk
    ParseChunkSize(context);

    if (this->IsEOF())
    {
      /* For a chunk response, EOF means that the last chunk was found.
       *  As per RFC, after the last chunk, there should be one last CRLF
       */
      ReadCRLF(context);
      // after parsing next chunk, check if it is zero
      return 0;
    }
  }

  auto totalRead = size_t();
  size_t readRequestLength = this->m_isChunkedResponseType
      ? (std::min)(this->m_chunkSize - this->m_sessionTotalRead, count)
      : count;

  // For responses with content-length, avoid trying to read beyond Content-length or
  // libcurl could return a second response as BadRequest.
  // https://github.com/Azure/azure-sdk-for-cpp/issues/306
  if (this->m_contentLength > 0)
  {
    size_t remainingBodyContent
        = static_cast<size_t>(this->m_contentLength) - this->m_sessionTotalRead;
    readRequestLength = (std::min)(readRequestLength, remainingBodyContent);
  }

  // Take data from inner buffer if any
  if (this->m_bodyStartInBuffer < this->m_innerBufferSize)
  {
    // still have data to take from innerbuffer
    Azure::Core::IO::MemoryBodyStream innerBufferMemoryStream(
        this->m_readBuffer + this->m_bodyStartInBuffer,
        this->m_innerBufferSize - this->m_bodyStartInBuffer);

    // From code inspection, it is guaranteed that the readRequestLength will fit within size_t
    // since count is bounded by size_t.
    totalRead = innerBufferMemoryStream.Read(buffer, readRequestLength, context);
    this->m_bodyStartInBuffer += totalRead;
    this->m_sessionTotalRead += totalRead;

    return totalRead;
  }

  // Head request have contentLength = 0, so we won't read more, just return 0
  // Also if we have already read all contentLength
  if (this->m_sessionTotalRead == static_cast<size_t>(this->m_contentLength) || this->IsEOF())
  {
    return 0;
  }

  // If we no longer have a connection, read 0 bytes.
  if (!m_connection)
  {
    return 0;
  }
  // Read from socket when no more data on internal buffer
  // For chunk request, read a chunk based on chunk size
  totalRead = m_connection->ReadFromSocket(buffer, static_cast<size_t>(readRequestLength), context);
  this->m_sessionTotalRead += totalRead;

  // Reading 0 bytes means closed connection.
  // For known content length and chunked response, this means there is nothing else to read
  // from server or lost connection before getting full response. For unknown response size,
  // it means the end of response and it's fine.
  if (totalRead == 0 && (this->m_contentLength > 0 || this->m_isChunkedResponseType))
  {
    auto expectedToRead = this->m_isChunkedResponseType ? this->m_chunkSize : this->m_contentLength;
    if (this->m_sessionTotalRead < expectedToRead)
    {
      throw TransportException(
          "Connection closed before getting full response or response is less than "
          "expected. "
          "Expected response length = "
          + std::to_string(expectedToRead)
          + ". Read until now = " + std::to_string(this->m_sessionTotalRead));
    }
  }

  return totalRead;
}

// Read from socket and return the number of bytes taken from socket
size_t CurlConnection::ReadFromSocket(uint8_t* buffer, size_t bufferSize, Context const& context)
{
  // loop until read result is not CURLE_AGAIN
  // Next loop is expected to be called at most 2 times:
  // The first time it calls `curl_easy_recv()`, if it returns CURLE_AGAIN it would call
  // `pollSocketUntilEventOrTimeout` and wait for socket to be ready to read.
  // `pollSocketUntilEventOrTimeout` will then handle cancelation token.
  // If socket is not ready before the timeout, Exception is thrown.
  // When socket is ready, it calls curl_easy_recv() again (second loop iteration). It is
  // not expected to return CURLE_AGAIN (since socket is ready), so, a chuck of data will be
  // downloaded and result will be CURLE_OK which breaks the loop. Also, getting other than
  // CURLE_OK or CURLE_AGAIN throws.
  size_t readBytes = 0;
  for (CURLcode readResult = CURLE_AGAIN; readResult == CURLE_AGAIN;)
  {
    readResult = curl_easy_recv(m_handle.get(), buffer, bufferSize, &readBytes);
    switch (readResult)
    {
      case CURLE_AGAIN: {
        // start polling operation
        auto pollUntilSocketIsReady = pollSocketUntilEventOrTimeout(
            context, m_curlSocket, PollSocketDirection::Read, 60000L);

        if (pollUntilSocketIsReady == 0)
        {
          throw TransportException("Timeout waiting for socket to read.");
        }
        else if (pollUntilSocketIsReady < 0)
        { // negative value, error while polling
          throw TransportException("Error while polling for socket ready read");
        }

        // Ready to continue download.
        break;
      }
      case CURLE_OK: {
        break;
      }
      default: {
        // Error reading from socket
        throw TransportException(
            "Error while reading from network socket. CURLE code: " + std::to_string(readResult)
            + ". " + std::string(curl_easy_strerror(readResult)));
      }
    }
  }
#if defined(AZ_PLATFORM_WINDOWS)
  WinSocketSetBuffSize(m_curlSocket);
#endif
  return readBytes;
}

std::unique_ptr<RawResponse> CurlSession::ExtractResponse() { return std::move(this->m_response); }

size_t CurlSession::ResponseBufferParser::Parse(
    uint8_t const* const buffer,
    size_t const bufferSize)
{
  if (this->m_parseCompleted)
  {
    return 0;
  }

  // Read all buffer until \r\n is found
  size_t start = 0, index = 0;
  for (; index < bufferSize; index++)
  {
    if (buffer[index] == '\r')
    {
      this->m_delimiterStartInPrevPosition = true;
      continue;
    }

    if (buffer[index] == '\n' && this->m_delimiterStartInPrevPosition)
    {
      // found end of delimiter
      if (this->m_internalBuffer.size() > 0) // Check internal buffer
      {
        // At this point, we are reading to append more to internal buffer.
        // Only append more if index is greater than 1, meaning not when buffer is [\r\nxxx]
        // only on buffer like [xxx\r\n yyyy], append xxx
        if (index > 1)
        {
          // Previously appended something
          this->m_internalBuffer.append(buffer + start, buffer + index - 1); // minus 1 to remove \r
        }
        if (this->state == ResponseParserState::StatusLine)
        {
          // Create Response
          this->m_response = CreateHTTPResponse(this->m_internalBuffer);
          // Set state to headers
          this->state = ResponseParserState::Headers;
          this->m_delimiterStartInPrevPosition = false;
          start = index + 1; // jump \n
        }
        else if (this->state == ResponseParserState::Headers)
        {
          // will throw if header is invalid
          SetHeader(*this->m_response, this->m_internalBuffer);
          this->m_delimiterStartInPrevPosition = false;
          start = index + 1; // jump \n
        }
        else
        {
          // Should never happen that parser is not statusLIne or Headers and we still try
          // to parse more.
          AZURE_UNREACHABLE_CODE();
        }
        // clean internal buffer
        this->m_internalBuffer.clear();
      }
      else
      {
        // Nothing at internal buffer. Add directly from internal buffer
        if (this->state == ResponseParserState::StatusLine)
        {
          // Create Response
          this->m_response = CreateHTTPResponse(buffer + start, buffer + index - 1);
          // Set state to headers
          this->state = ResponseParserState::Headers;
          this->m_delimiterStartInPrevPosition = false;
          start = index + 1; // jump \n
        }
        else if (this->state == ResponseParserState::Headers)
        {
          // Check if this is end of headers delimiter
          // 1) internal buffer is empty and \n is the first char on buffer [\nBody...]
          // 2) index == start + 1. No header data after last \r\n [header\r\n\r\n]
          if (index == 0 || index == start + 1)
          {
            this->m_parseCompleted = true;
            return index + 1; // plus 1 to advance the \n. If we were at buffer end.
          }

          // will throw if header is invalid
          Azure::Core::Http::_detail::RawResponseHelpers::SetHeader(
              *this->m_response, buffer + start, buffer + index - 1);
          this->m_delimiterStartInPrevPosition = false;
          start = index + 1; // jump \n
        }
        else
        {
          // Should never happen that parser is not statusLIne or Headers and we still try
          // to parse more.
          AZURE_UNREACHABLE_CODE();
        }
      }
    }
    else
    {
      if (index == 0 && this->m_internalBuffer.size() > 0 && this->m_delimiterStartInPrevPosition)
      {
        // unlikely. But this means a case with buffers like [xx\r], [xxxx]
        // \r is not delimiter and in previous loop it was omitted, so adding it now
        this->m_internalBuffer.append("\r");
      }
      // \r in the response without \n after it. keep parsing
      this->m_delimiterStartInPrevPosition = false;
    }
  }

  if (start < bufferSize)
  {
    // didn't find the end of delimiter yet, save at internal buffer
    // If this->m_delimiterStartInPrevPosition is true, buffer ends in \r [xxxx\r]
    // Don't add \r. IF next char is not \n, we will append \r then on next loop
    this->m_internalBuffer.append(
        buffer + start, buffer + bufferSize - (this->m_delimiterStartInPrevPosition ? 1 : 0));
  }

  return index;
}

namespace {
// Calculate the connection key.
// The connection key is a tuple of host, proxy info, TLS info, etc. Basically any characteristics
// of the connection that should indicate that the connection shouldn't be re-used should be listed
// the connection key.
inline std::string GetConnectionKey(std::string const& host, CurlTransportOptions const& options)
{
  std::string key(host);
  key.append(",");
  key.append(!options.CAInfo.empty() ? options.CAInfo : "0");
  key.append(",");
  key.append(!options.CAPath.empty() ? options.CAPath : "0");
  key.append(",");
  key.append(
      options.Proxy.HasValue() ? (options.Proxy.Value().empty() ? "NoProxy" : options.Proxy.Value())
                               : "0");
  key.append(",");
  key.append(options.ProxyUsername.ValueOr("0"));
  key.append(",");
  key.append(options.ProxyPassword.ValueOr("0"));
  key.append(",");
  key.append(!options.SslOptions.EnableCertificateRevocationListCheck ? "1" : "0");
  key.append(",");
  key.append(options.SslVerifyPeer ? "1" : "0");
  key.append(",");
  key.append(options.NoSignal ? "1" : "0");
  key.append(",");
  key.append(options.SslOptions.AllowFailedCrlRetrieval ? "FC" : "0");
  key.append(",");
#if LIBCURL_VERSION_NUM >= 0x074D00 // 7.77.0
  key.append(
      !options.SslOptions.PemEncodedExpectedRootCertificates.empty() ? std::to_string(
          std::hash<std::string>{}(options.SslOptions.PemEncodedExpectedRootCertificates))
                                                                     : "0");
#else
  key.append("0");
#endif
  key.append(",");
  // using DefaultConnectionTimeout or 0 result in the same setting
  key.append(
      (options.ConnectionTimeout == Azure::Core::Http::_detail::DefaultConnectionTimeout
       || options.ConnectionTimeout == std::chrono::milliseconds(0))
          ? "0"
          : std::to_string(options.ConnectionTimeout.count()));

  return key;
}

void DumpCurlInfoToLog(std::string const& text, uint8_t* ptr, size_t size)
{

  size_t width = 0x10;

  std::stringstream ss;
  ss << text << ", " << std::dec << std::setw(10) << std::setfill('0') << size << " bytes: (0x"
     << std::setw(8) << std::hex << size << ")";

  Log::Write(Logger::Level::Verbose, ss.str());

  for (size_t i = 0; i < size; i += width)
  {
    ss = std::stringstream();
    ss << std::hex << std::setw(4) << i << ": ";

    /* hex not disabled, show it */
    for (size_t c = 0; c < width; c++)
    {
      if (i + c < size)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ptr[i + c]) << " ";
      else
        ss << "   ";
    }
    for (size_t c = 0; (c < width) && (i + c < size); c++)
    {
      // Log the contents of the buffer as text, if it's printable, print the character, otherwise
      // print '.'
      auto const ch = static_cast<char>(ptr[i + c]);
      if (Azure::Core::_internal::StringExtensions::IsPrintable(ch))
      {
        ss << ch;
      }
      else
      {
        ss << ".";
      }
    }

    Log::Write(Logger::Level::Verbose, ss.str());
  }
}

} // namespace

int CurlConnection::CurlLoggingCallback(CURL*, curl_infotype type, char* data, size_t size, void*)
{
  if (type == CURLINFO_TEXT)
  {
    std::string textToLog{data};
    // If the last character to log is a \n, remove it because Log::Write will append a \n.
    if (textToLog.back() == '\n')
    {
      textToLog.resize(textToLog.size() - 1);
    }
    Log::Write(Logger::Level::Verbose, "== Info: " + textToLog);
  }
  else
  {
    std::string prefix;

    switch (type)
    {
      case CURLINFO_HEADER_OUT:
        prefix = "=> Send header";
        break;
      case CURLINFO_DATA_OUT:
        prefix = "=> Send data";
        break;
      case CURLINFO_SSL_DATA_OUT:
        prefix = "=> Send SSL data";
        break;
      case CURLINFO_HEADER_IN:
        prefix = "<= Recv header";
        break;
      case CURLINFO_DATA_IN:
        prefix = "<= Recv data";
        break;
      case CURLINFO_SSL_DATA_IN:
        prefix = "<= Recv SSL data";
        break;
      default: /* in case a new one is introduced to shock us */
        return 0;
    }
    DumpCurlInfoToLog(prefix, reinterpret_cast<uint8_t*>(data), size);
  }
  return 0;
}

// On Windows and macOS, libcurl uses native crypto backends, this functionality depends on
// the OpenSSL backend.
#if !defined(AZ_PLATFORM_WINDOWS) && !defined(AZ_PLATFORM_MAC)
namespace Azure { namespace Core {
  namespace _detail {

    template <> struct UniqueHandleHelper<X509>
    {
      using type = _internal::BasicUniqueHandle<X509, X509_free>;
    };
    template <> struct UniqueHandleHelper<X509_CRL>
    {
      using type = _internal::BasicUniqueHandle<X509_CRL, X509_CRL_free>;
    };

    template <> struct UniqueHandleHelper<BIO>
    {
      using type = _internal::BasicUniqueHandle<BIO, BIO_free_all>;
    };
#if defined(USE_OPENSSL_1)
    template <> struct UniqueHandleHelper<OCSP_REQ_CTX>
    {
      using type = _internal::BasicUniqueHandle<OCSP_REQ_CTX, OCSP_REQ_CTX_free>;
    };
#endif // USE_OPENSSL_1

    template <> struct UniqueHandleHelper<STACK_OF(X509_CRL)>
    {
      static void FreeCrlStack(STACK_OF(X509_CRL) * obj)
      {
        sk_X509_CRL_pop_free(obj, X509_CRL_free);
      }
      using type = _internal::BasicUniqueHandle<STACK_OF(X509_CRL), FreeCrlStack>;
    };

    template <typename Api, typename... Args> auto MakeUniqueHandle(Api& OpensslApi, Args&&... args)
    {
      auto raw = OpensslApi(std::forward<Args>(
          args)...); // forwarding is probably unnecessary, could use const Args&...
      // check raw
      using T = std::remove_pointer_t<decltype(raw)>; // no need to request T when we can see
                                                      // what OpensslApi returned
      return _internal::UniqueHandle<T>{raw};
    }
  } // namespace _detail

  namespace Http {
    namespace _detail {

      // Disable Code Coverage across GetOpenSSLError because we don't have a good way of forcing
      // OpenSSL to fail.

      std::string GetOpenSSLError(std::string const& what)
      {
        auto bio(Azure::Core::_detail::MakeUniqueHandle(BIO_new, BIO_s_mem()));

        BIO_printf(bio.get(), "Error in %hs: ", what.c_str());
        if (ERR_peek_error() != 0)
        {
          ERR_print_errors(bio.get());
        }
        else
        {
          BIO_printf(bio.get(), "Unknown error.");
        }

        uint8_t* bioData;
        long bufferSize = BIO_get_mem_data(bio.get(), &bioData);
        std::string returnValue;
        returnValue.resize(bufferSize);
        memcpy(&returnValue[0], bioData, bufferSize);

        return returnValue;
      }

    } // namespace _detail

    namespace {
      // int g_ssl_crl_max_size_in_kb = 20;
      /**
       * @brief The Cryptography class provides a set of basic cryptographic primatives required
       * by the attestation samples.
       */

      Azure::Core::_internal::UniqueHandle<X509_CRL> LoadCrlFromUrl(std::string const& url)
      {
        Log::Stream(Logger::Level::Informational) << "Load CRL from Url: " << url << std::endl;
        Azure::Core::_internal::UniqueHandle<X509_CRL> crl;
#if defined(USE_OPENSSL_3)
        crl = Azure::Core::_detail::MakeUniqueHandle(
            X509_CRL_load_http, url.c_str(), nullptr, nullptr, 5);
#else
        std::string host, port, path;
        int use_ssl;
        {
          char *host_ptr, *port_ptr, *path_ptr;
          if (!OCSP_parse_url(url.c_str(), &host_ptr, &port_ptr, &path_ptr, &use_ssl))
          {
            Log::Write(Logger::Level::Error, "Failure parsing URL");
            return nullptr;
          }
          host = host_ptr;
          port = port_ptr;
          path = path_ptr;
        }

        if (use_ssl)
        {
          Log::Write(Logger::Level::Error, "CRL HTTPS not supported");
          return nullptr;
        }
        Azure::Core::_internal::UniqueHandle<BIO> bio{
            Azure::Core::_detail::MakeUniqueHandle(BIO_new_connect, host.c_str())};
        if (!bio)
        {
          Log::Write(
              Logger::Level::Error,
              "BIO_new_connect failed" + _detail::GetOpenSSLError("Load CRL"));
          return nullptr;
        }
        if (!BIO_set_conn_port(bio.get(), const_cast<char*>(port.c_str())))
        {
          Log::Write(
              Logger::Level::Error,
              "BIO_set_conn_port failed" + _detail::GetOpenSSLError("Load CRL"));
          return nullptr;
        }

        auto requestContext
            = Azure::Core::_detail::MakeUniqueHandle(OCSP_REQ_CTX_new, bio.get(), 1024 * 1024);
        if (!requestContext)
        {
          Log::Write(
              Logger::Level::Error,
              "OCSP_REQ_CTX_new failed" + _detail::GetOpenSSLError("Load CRL"));
          return nullptr;
        }

        // By default the OCSP APIs limit the CRL length to 1M, that isn't sufficient
        // for many web sites, so increase it to 10M.
        OCSP_set_max_response_length(requestContext.get(), 10 * 1024 * 1024);

        if (!OCSP_REQ_CTX_http(requestContext.get(), "GET", url.c_str()))
        {
          Log::Write(
              Logger::Level::Error,
              "OCSP_REQ_CTX_http failed" + _detail::GetOpenSSLError("Load CRL"));
          return nullptr;
        }

        if (!OCSP_REQ_CTX_add1_header(requestContext.get(), "Host", host.c_str()))
        {
          Log::Write(
              Logger::Level::Error,
              "OCSP_REQ_add1_header failed" + _detail::GetOpenSSLError("Load CRL"));
          return nullptr;
        }

        {
          X509_CRL* crl_ptr = nullptr;
          int rv;
          do
          {
            rv = X509_CRL_http_nbio(requestContext.get(), &crl_ptr);
          } while (rv == -1);

          if (rv != 1)
          {
            if (ERR_peek_error() == 0)
            {
              Log::Write(
                  Logger::Level::Error,
                  "X509_CRL_http_nbio failed, possible because CRL is too long.");
            }
            else
            {
              Log::Write(
                  Logger::Level::Error,
                  "X509_CRL_http_nbio failed" + _detail::GetOpenSSLError("Load CRL"));
            }
            return nullptr;
          }
          crl.reset(crl_ptr);
        }

#endif
        if (!crl)
        {
          Log::Write(Logger::Level::Error, _detail::GetOpenSSLError("Load CRL"));
        }

        return crl;
      }

      enum class CrlFormat : int
      {
        Http,
        Asn1,
        PEM,
      };

      Azure::Core::_internal::UniqueHandle<X509_CRL> LoadCrl(
          std::string const& source,
          CrlFormat format)
      {
        Azure::Core::_internal::UniqueHandle<X509_CRL> x;
        Azure::Core::_internal::UniqueHandle<BIO> in;

        if (format == CrlFormat::Http)
        {
          return LoadCrlFromUrl(source);
        }
        return x;
      }

      bool IsCrlValid(X509_CRL* crl)
      {
        const ASN1_TIME* at = X509_CRL_get0_nextUpdate(crl);

        int day = -1;
        int sec = -1;
        if (!ASN1_TIME_diff(&day, &sec, nullptr, at))
        {
          Log::Write(Logger::Level::Error, "Could not check expiration");
          return false; /* Safe default, invalid */
        }

        if (day > 0 || sec > 0)
        {
          return true; /* Later, valid */
        }
        return false; /* Before or same, invalid */
      }

      const char* GetDistributionPointUrl(DIST_POINT* dp)
      {
        GENERAL_NAMES* gens;
        GENERAL_NAME* gen;
        int i, nameType;
        ASN1_STRING* uri;

        if (!dp->distpoint)
        {
          Log::Write(Logger::Level::Informational, "returning, dp->distpoint is null");
          return nullptr;
        }

        if (dp->distpoint->type != 0)
        {
          Log::Write(
              Logger::Level::Informational,
              "returning, dp->distpoint->type is " + std::to_string(dp->distpoint->type));
          return nullptr;
        }

        gens = dp->distpoint->name.fullname;

        for (i = 0; i < sk_GENERAL_NAME_num(gens); i++)
        {
          gen = sk_GENERAL_NAME_value(gens, i);
          uri = static_cast<ASN1_STRING*>(GENERAL_NAME_get0_value(gen, &nameType));

          if (nameType == GEN_URI && ASN1_STRING_length(uri) > 6)
          {
            const char* uptr = reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri));
            if (strncmp(uptr, "http://", 7) == 0)
            {
              return uptr;
            }
          }
        }

        return nullptr;
      }

      std::mutex crl_cache_lock;
      std::vector<X509_CRL*> crl_cache;

      bool SaveCertificateCrlToMemory(
          X509* cert,
          Azure::Core::_internal::UniqueHandle<X509_CRL> const& crl)
      {
        std::unique_lock<std::mutex> lockResult(crl_cache_lock);

        // update existing
        X509_NAME* cert_issuer = cert ? X509_get_issuer_name(cert) : nullptr;
        for (auto it = crl_cache.begin(); it != crl_cache.end(); ++it)
        {
          X509_CRL* cacheEntry = *it;
          if (!cacheEntry)
          {
            continue;
          }

          X509_NAME* crl_issuer = X509_CRL_get_issuer(cacheEntry);
          if (!crl_issuer || !cert_issuer)
          {
            continue;
          }

          // If we are getting a new CRL for an existing CRL, update the
          // CRL with the new CRL.
          if (0 == X509_NAME_cmp(crl_issuer, cert_issuer))
          {
            // Bump the refcount on the new CRL before adding it to the cache.
            X509_CRL_free(*it);
            X509_CRL_up_ref(crl.get());
            *it = crl.get();
            return true;
          }
        }

        // not found, so try to find slot by purging outdated
        for (auto it = crl_cache.begin(); it != crl_cache.end(); ++it)
        {
          if (!*it)
          {
            // set new
            X509_CRL_free(*it);
            X509_CRL_up_ref(crl.get());
            *it = crl.get();
            return true;
          }

          if (!IsCrlValid(*it))
          {
            // remove stale
            X509_CRL_free(*it);
            X509_CRL_up_ref(crl.get());
            *it = crl.get();
            return true;
          }
        }

        // Clone the certificate and add it to the cache.
        X509_CRL_up_ref(crl.get());
        crl_cache.push_back(crl.get());
        return true;
      }

      Azure::Core::_internal::UniqueHandle<X509_CRL> LoadCertificateCrlFromMemory(X509* cert)
      {
        X509_NAME* cert_issuer = cert ? X509_get_issuer_name(cert) : nullptr;

        std::unique_lock<std::mutex> lockResult(crl_cache_lock);

        for (auto it = crl_cache.begin(); it != crl_cache.end(); ++it)
        {
          X509_CRL* crl = *it;
          if (!*it)
          {
            continue;
          }

          // names don't match up. probably a hash collision
          // so lets test if there is another crl on disk.
          X509_NAME* crl_issuer = X509_CRL_get_issuer(crl);
          if (!crl_issuer || !cert_issuer)
          {
            continue;
          }

          if (0 != X509_NAME_cmp(crl_issuer, cert_issuer))
          {
            continue;
          }

          if (!IsCrlValid(crl))
          {
            Log::Write(Logger::Level::Informational, "Discarding outdated CRL");
            X509_CRL_free(*it);
            *it = nullptr;
            continue;
          }

          X509_CRL_up_ref(crl);
          return Azure::Core::_internal::UniqueHandle<X509_CRL>(crl);
        }
        return nullptr;
      }

      Azure::Core::_internal::UniqueHandle<X509_CRL> LoadCrlFromCacheAndDistributionPoint(
          X509* cert,
          STACK_OF(DIST_POINT) * crlDistributionPointStack)
      {
        int i;

        Azure::Core::_internal::UniqueHandle<X509_CRL> crl = LoadCertificateCrlFromMemory(cert);
        if (crl)
        {
          return crl;
        }

        // file was not found on disk cache,
        // so, now loading from web.
        // Walk through the possible CRL distribution points
        // looking for one which has a URL that we can download.
        const char* urlptr = nullptr;
        for (i = 0; i < sk_DIST_POINT_num(crlDistributionPointStack); i++)
        {
          DIST_POINT* dp = sk_DIST_POINT_value(crlDistributionPointStack, i);

          urlptr = GetDistributionPointUrl(dp);
          if (urlptr)
          {
            // try to load from web, exit loop if
            // successfully downloaded
            crl = LoadCrl(urlptr, CrlFormat::Http);
            if (crl)
              break;
          }
        }

        if (!urlptr)
        {
          Log::Write(Logger::Level::Error, "No CRL dist point qualified for downloading.");
        }

        if (crl)
        {
          // save it to memory
          SaveCertificateCrlToMemory(cert, crl);
        }

        return crl;
      }

      /**
       * @brief Retrieve the CRL associated with the provided store context, if available.
       *
       */
#if defined(USE_OPENSSL_3)
      STACK_OF(X509_CRL) * CrlHttpCallback(const X509_STORE_CTX* context, const X509_NAME*)
#else
      STACK_OF(X509_CRL) * CrlHttpCallback(X509_STORE_CTX* context, X509_NAME*)
#endif
      {
        Azure::Core::_internal::UniqueHandle<X509_CRL> crl;
        STACK_OF(DIST_POINT) * crlDistributionPoint;

        Azure::Core::_internal::UniqueHandle<STACK_OF(X509_CRL)> crlStack
            = Azure::Core::_internal::UniqueHandle<STACK_OF(X509_CRL)>(sk_X509_CRL_new_null());
        if (crlStack == nullptr)
        {
          Log::Write(Logger::Level::Error, "Failed to allocate STACK_OF(X509_CRL)");
          return nullptr;
        }

        X509* currentCertificate = X509_STORE_CTX_get_current_cert(context);

        // try to download Crl
        crlDistributionPoint = static_cast<STACK_OF(DIST_POINT)*>(
            X509_get_ext_d2i(currentCertificate, NID_crl_distribution_points, nullptr, nullptr));
        if (!crlDistributionPoint
            && X509_NAME_cmp(
                   X509_get_issuer_name(currentCertificate),
                   X509_get_subject_name(currentCertificate))
                != 0)
        {
          Log::Write(
              Logger::Level::Error,
              "No CRL distribution points defined on non self-issued cert, CRL check may fail.");
          return nullptr;
        }

        crl = LoadCrlFromCacheAndDistributionPoint(currentCertificate, crlDistributionPoint);

        sk_DIST_POINT_pop_free(crlDistributionPoint, DIST_POINT_free);
        if (!crl)
        {
          Log::Write(Logger::Level::Error, "Unable to retrieve CRL, CRL check may fail.");
          return nullptr;
        }

        sk_X509_CRL_push(crlStack.get(), X509_CRL_dup(crl.get()));

        // try to download delta Crl
        crlDistributionPoint = static_cast<STACK_OF(DIST_POINT)*>(
            X509_get_ext_d2i(currentCertificate, NID_freshest_crl, nullptr, nullptr));
        if (crlDistributionPoint != nullptr)
        {
          crl = LoadCrlFromCacheAndDistributionPoint(currentCertificate, crlDistributionPoint);

          sk_DIST_POINT_pop_free(crlDistributionPoint, DIST_POINT_free);
          if (crl)
          {
            sk_X509_CRL_push(crlStack.get(), X509_CRL_dup(crl.get()));
          }
        }

        return crlStack.release();
      }

      int GetOpenSSLContextConnectionIndex()
      {
        static int openSslConnectionIndex = -1;
        if (openSslConnectionIndex < 0)
        {
          openSslConnectionIndex
              = X509_STORE_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        }
        return openSslConnectionIndex;
      }
      int GetOpenSSLContextLastVerifyFunction()
      {
        static int openSslLastVerifyFunctionIndex = -1;
        if (openSslLastVerifyFunctionIndex < 0)
        {
          openSslLastVerifyFunctionIndex
              = X509_STORE_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        }
        return openSslLastVerifyFunctionIndex;
      }
    } // namespace
  } // namespace Http
}} // namespace Azure::Core

// OpenSSL X509 Certificate Validation function - based off of the example found at:
// https://linux.die.net/man/3/x509_store_ctx_set_verify_cb
//
int CurlConnection::VerifyCertificateError(int ok, X509_STORE_CTX* storeContext)
{
  X509_STORE* certStore = X509_STORE_CTX_get0_store(storeContext);
  X509* err_cert;
  int err, depth;
  Azure::Core::_internal::UniqueHandle<BIO> bio_err(
      Azure::Core::_detail::MakeUniqueHandle(BIO_new, BIO_s_mem()));

  err_cert = X509_STORE_CTX_get_current_cert(storeContext);
  err = X509_STORE_CTX_get_error(storeContext);
  depth = X509_STORE_CTX_get_error_depth(storeContext);

  BIO_printf(bio_err.get(), "depth=%d ", depth);
  if (err_cert)
  {
    X509_NAME_print_ex(bio_err.get(), X509_get_subject_name(err_cert), 0, XN_FLAG_ONELINE);
    BIO_puts(bio_err.get(), "\n");
  }
  else
  {
    BIO_puts(bio_err.get(), "<no cert>\n");
  }
  if (!ok)
  {
    BIO_printf(bio_err.get(), "verify error:num=%d: %s\n", err, X509_verify_cert_error_string(err));
  }

  switch (err)
  {
    case X509_V_ERR_UNABLE_TO_GET_CRL:
      BIO_printf(bio_err.get(), "Unable to retrieve CRL.");
      break;
  }
  if (err == X509_V_OK && ok == 2)
  {
    /* print out policies */
    BIO_printf(bio_err.get(), "verify return:%d\n", ok);
  }

  //  Handle certificate specific errors here based on configuration options.
  {
    if (err == X509_V_ERR_UNABLE_TO_GET_CRL)
    {
      if (m_allowFailedCrlRetrieval)
      {
        BIO_printf(bio_err.get(), "Ignoring CRL retrieval error by configuration.\n");
        // Clear the X509 error in the store context, because CURL retrieves it,
        // and it overwrites the successful result.
        X509_STORE_CTX_set_error(storeContext, X509_V_OK);
        // Return true, indicating that things are all good.
        ok = 1;
      }
      else
      {
        BIO_printf(
            bio_err.get(), "Fail TLS negotiation because CRL retrieval is not configured.\n");
      }
    }
  }

  char outputString[128];
  int len;
  while ((len = BIO_gets(bio_err.get(), outputString, sizeof(outputString))) >= 0)
  {
    if (len == 0)
    {
      break;
    }
    if (outputString[len - 1] == '\n')
    {
      outputString[len - 1] = '\0';
    }
    Log::Write(Logger::Level::Informational, std::string(outputString));
  }

  if (ok)
  {
    // We've done our stuff, call the pre-existing callback.
    auto existingCallback = reinterpret_cast<X509_STORE_CTX_verify_cb>(
        X509_STORE_get_ex_data(certStore, GetOpenSSLContextLastVerifyFunction()));
    if (existingCallback != nullptr)
    {
      ok = existingCallback(ok, storeContext);
    }
  }
  return (ok);
}

int CurlConnection::CurlSslCtxCallback(CURL* curl, void* sslctx, void* parm)
{
  CurlConnection* connection = static_cast<CurlConnection*>(parm);
  return connection->SslCtxCallback(curl, sslctx);
}

int CurlConnection::SslCtxCallback(CURL*, void* sslctx)
{
  SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(sslctx);

  // Note: SSL_CTX_get_cert_store does NOT increase the store reference count.
  X509_STORE* certStore = SSL_CTX_get_cert_store(ctx);
  X509_VERIFY_PARAM* verifyParam = X509_STORE_get0_param(certStore);
  if (m_enableCrlValidation)
  {

    // Store our connection handle in the store extended data so it can be retrieved
    // in later callbacks. This allows setting options on a per-connection basis.
    X509_STORE_set_ex_data(certStore, GetOpenSSLContextConnectionIndex(), this);

    X509_VERIFY_PARAM_set_flags(verifyParam, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
    X509_STORE_set_lookup_crls_cb(certStore, CrlHttpCallback);

    X509_STORE_set_ex_data(
        certStore,
        GetOpenSSLContextLastVerifyFunction(),
        reinterpret_cast<void*>(X509_STORE_get_verify_cb(certStore)));

    X509_STORE_set_verify_cb(certStore, [](int ok, X509_STORE_CTX* storeContext) {
      X509_STORE* certStore = X509_STORE_CTX_get0_store(storeContext);

      CurlConnection* thisConnection = reinterpret_cast<CurlConnection*>(
          X509_STORE_get_ex_data(certStore, GetOpenSSLContextConnectionIndex()));

      return thisConnection->VerifyCertificateError(ok, storeContext);
    });
  }
  else
  {
    X509_VERIFY_PARAM_clear_flags(verifyParam, X509_V_FLAG_CRL_CHECK);
  }
  return CURLE_OK;
}
#endif

std::unique_ptr<CurlNetworkConnection> CurlConnectionPool::ExtractOrCreateCurlConnection(
    Request& request,
    CurlTransportOptions const& options,
    bool resetPool)
{
  uint16_t port = request.GetUrl().GetPort();
  // Generate a display name for the host being connected to
  std::string const& hostDisplayName = request.GetUrl().GetScheme() + "://"
      + request.GetUrl().GetHost() + (port != 0 ? ":" + std::to_string(port) : "");
  std::string const connectionKey = GetConnectionKey(hostDisplayName, options);

  {
    decltype(CurlConnectionPool::g_curlConnectionPool
                 .ConnectionPoolIndex)::mapped_type connectionsToBeReset;

    // Critical section. Needs to own ConnectionPoolMutex before executing
    // Lock mutex to access connection pool. mutex is unlock as soon as lock is out of scope
    std::unique_lock<std::mutex> lock(CurlConnectionPool::ConnectionPoolMutex);

    // get a ref to the pool from the map of pools
    auto hostPoolIndex = g_curlConnectionPool.ConnectionPoolIndex.find(connectionKey);

    if (hostPoolIndex != g_curlConnectionPool.ConnectionPoolIndex.end()
        && hostPoolIndex->second.size() > 0)
    {
      if (resetPool)
      {
        connectionsToBeReset = std::move(hostPoolIndex->second);
        // clean the pool-index as requested in the call. Typically to force a new connection to be
        // created and to discard all current connections in the pool for the host-index. A caller
        // might request this after getting broken/closed connections multiple-times.
        hostPoolIndex->second.clear();
        Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Reset connection pool requested.");
      }
      else
      {
        // get ref to first connection
        auto fistConnectionIterator = hostPoolIndex->second.begin();
        // move the connection ref to temp ref
        auto connection = std::move(*fistConnectionIterator);
        // Remove the connection ref from list
        hostPoolIndex->second.erase(fistConnectionIterator);

        // Remove index if there are no more connections
        if (hostPoolIndex->second.size() == 0)
        {
          g_curlConnectionPool.ConnectionPoolIndex.erase(hostPoolIndex);
        }

        Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Re-using connection from the pool.");
        // return connection ref
        return connection;
      }
    }
  }

  // Creating a new connection is thread safe. No need to lock mutex here.
  // No available connection for the pool for the required host. Create one
  Log::Write(Logger::Level::Verbose, LogMsgPrefix + "Spawn new connection.");

  return std::make_unique<CurlConnection>(request, options, hostDisplayName, connectionKey);
}

// Move the connection back to the connection pool. Push it to the front so it becomes the
// first connection to be picked next time some one ask for a connection to the pool (LIFO)
void CurlConnectionPool::MoveConnectionBackToPool(
    std::unique_ptr<CurlNetworkConnection> connection,
    bool httpKeepAlive)
{
  if (!httpKeepAlive)
  {
    return; // The server has asked us to not re-use this connection.
  }

  if (connection->IsShutdown())
  {
    // Can't re-used a shut down connection
    return;
  }

  Log::Write(Logger::Level::Verbose, "Moving connection to pool...");

  decltype(CurlConnectionPool::g_curlConnectionPool
               .ConnectionPoolIndex)::mapped_type::value_type connectionToBeRemoved;

  // Lock mutex to access connection pool. mutex is unlock as soon as lock is out of scope
  std::unique_lock<std::mutex> lock(CurlConnectionPool::ConnectionPoolMutex);
  auto& poolId = connection->GetConnectionKey();
  auto& hostPool = g_curlConnectionPool.ConnectionPoolIndex[poolId];

  if (hostPool.size() >= _detail::MaxConnectionsPerIndex && !hostPool.empty())
  {
    // Remove the last connection from the pool to insert this one.
    auto lastConnection = --hostPool.end();
    connectionToBeRemoved = std::move(*lastConnection);
    hostPool.erase(lastConnection);
  }

  // update the time when connection was moved back to pool
  connection->UpdateLastUsageTime();
  hostPool.push_front(std::move(connection));

  if (m_cleanThread.joinable() && !IsCleanThreadRunning)
  {
    // Clean thread was running before but it's finished, join it to finalize
    m_cleanThread.join();
  }

  // Cleanup will start a background thread which will close abandoned connections from the pool.
  // This will free-up resources from the app
  // This is the only call to cleanup.
  if (!m_cleanThread.joinable())
  {
    Log::Write(Logger::Level::Verbose, "Start clean thread");
    IsCleanThreadRunning = true;
    m_cleanThread = std::thread(CleanupThread);
  }
  else
  {
    Log::Write(Logger::Level::Verbose, "Clean thread running. Won't start a new one.");
  }
}

CurlConnection::CurlConnection(
    Request& request,
    CurlTransportOptions const& options,
    std::string const& hostDisplayName,
    std::string const& connectionPropertiesKey)
    : m_connectionKey(connectionPropertiesKey)
{
  m_handle = Azure::Core::_internal::UniqueHandle<CURL>(curl_easy_init());
  if (!m_handle)
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName + ". "
        + std::string("curl_easy_init returned Null"));
  }
  CURLcode result;

  if (options.EnableCurlTracing)
  {
    if (!SetLibcurlOption(
            m_handle, CURLOPT_DEBUGFUNCTION, CurlConnection::CurlLoggingCallback, &result))
    {
      throw TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate
          + std::string(". Could not enable logging callback.")
          + std::string(curl_easy_strerror(result)));
    }
    if (!SetLibcurlOption(m_handle, CURLOPT_VERBOSE, 1, &result))
    {
      throw TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate
          + std::string(". Could not enable verbose logging.")
          + std::string(curl_easy_strerror(result)));
    }
  }

  // Libcurl setup before open connection (url, connect_only, timeout)
  if (!SetLibcurlOption(m_handle, CURLOPT_URL, request.GetUrl().GetAbsoluteUrl().data(), &result))
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName + ". "
        + std::string(curl_easy_strerror(result)));
  }

  if (request.GetUrl().GetPort() != 0
      && !SetLibcurlOption(m_handle, CURLOPT_PORT, request.GetUrl().GetPort(), &result))
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName + ". "
        + std::string(curl_easy_strerror(result)));
  }

  if (!SetLibcurlOption(m_handle, CURLOPT_CONNECT_ONLY, 1L, &result))
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName + ". "
        + std::string(curl_easy_strerror(result)));
  }

  //   Set timeout to 24h. Libcurl will fail uploading on windows if timeout is:
  // timeout >= 25 days. Fails as soon as trying to upload any data
  // 25 days < timeout > 1 days. Fail on huge uploads ( > 1GB)
  if (!SetLibcurlOption(m_handle, CURLOPT_TIMEOUT, 60L * 60L * 24L, &result))
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName + ". "
        + std::string(curl_easy_strerror(result)));
  }

  if (options.ConnectionTimeout != Azure::Core::Http::_detail::DefaultConnectionTimeout)
  {
    if (!SetLibcurlOption(m_handle, CURLOPT_CONNECTTIMEOUT_MS, options.ConnectionTimeout, &result))
    {
      throw Azure::Core::Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Fail setting connect timeout to: "
          + std::to_string(options.ConnectionTimeout.count()) + " ms. "
          + std::string(curl_easy_strerror(result)));
    }
  }

  /******************** Curl handle options apply to all connections created
   * The keepAlive option is managed by the session directly.
   */
  if (options.Proxy)
  {
    if (!SetLibcurlOption(m_handle, CURLOPT_PROXY, options.Proxy->c_str(), &result))
    {
      throw Azure::Core::Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set proxy to:" + options.Proxy.Value() + ". "
          + std::string(curl_easy_strerror(result)));
    }
  }

  if (options.ProxyUsername.HasValue())
  {
    if (!SetLibcurlOption(
            m_handle, CURLOPT_PROXYUSERNAME, options.ProxyUsername.Value().c_str(), &result))
    {
      throw TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set proxy username to:" + options.ProxyUsername.Value() + ". "
          + std::string(curl_easy_strerror(result)));
    }
  }
  if (options.ProxyPassword.HasValue())
  {
    if (!SetLibcurlOption(
            m_handle, CURLOPT_PROXYPASSWORD, options.ProxyPassword.Value().c_str(), &result))
    {
      throw TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set proxy password to:" + options.ProxyPassword.Value() + ". "
          + std::string(curl_easy_strerror(result)));
    }
  }

  if (!options.CAInfo.empty())
  {
    if (!SetLibcurlOption(m_handle, CURLOPT_CAINFO, options.CAInfo.c_str(), &result))
    {
      throw Azure::Core::Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set CA cert file to:" + options.CAInfo + ". "
          + std::string(curl_easy_strerror(result)));
    }
  }

  if (!options.CAPath.empty())
  {
    if (!SetLibcurlOption(m_handle, CURLOPT_CAPATH, options.CAPath.c_str(), &result))
    {
      throw Azure::Core::Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set CA path to:" + options.CAPath + ". "
          + std::string(curl_easy_strerror(result)));
    }
  }

#if LIBCURL_VERSION_NUM >= 0x074D00 // 7.77.0
  if (!options.SslOptions.PemEncodedExpectedRootCertificates.empty())
  {
    curl_blob rootCertBlob
        = {const_cast<void*>(reinterpret_cast<const void*>(
               options.SslOptions.PemEncodedExpectedRootCertificates.c_str())),
           options.SslOptions.PemEncodedExpectedRootCertificates.size(),
           CURL_BLOB_COPY};
    if (!SetLibcurlOption(m_handle, CURLOPT_CAINFO_BLOB, &rootCertBlob, &result))
    {
      throw Azure::Core::Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set CA cert to:" + options.CAInfo + ". "
          + std::string(curl_easy_strerror(result)));
    }
  }
#endif

#if defined(AZ_PLATFORM_WINDOWS)
  long sslOption = 0;
  if (!options.SslOptions.EnableCertificateRevocationListCheck)
  {
    sslOption |= CURLSSLOPT_NO_REVOKE;
  }

  if (!SetLibcurlOption(m_handle, CURLOPT_SSL_OPTIONS, sslOption, &result))
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
        + ". Failed to set ssl options to long bitmask:" + std::to_string(sslOption) + ". "
        + std::string(curl_easy_strerror(result)));
  }
#elif !defined(AZ_PLATFORM_MAC)
  if (options.SslOptions.EnableCertificateRevocationListCheck)
  {
    if (!SetLibcurlOption(
            m_handle, CURLOPT_SSL_CTX_FUNCTION, CurlConnection::CurlSslCtxCallback, &result))
    {
      throw TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set SSL context callback. " + std::string(curl_easy_strerror(result)));
    }
    if (!SetLibcurlOption(m_handle, CURLOPT_SSL_CTX_DATA, this, &result))
    {
      throw TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set SSL context callback data. "
          + std::string(curl_easy_strerror(result)));
    }
    //          if (!SetLibcurlOption(m_handle, CURLOPT_SSL_VERIFYSTATUS, 1, &result))
    //          {
    //            throw TransportException(
    //                _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
    //                + ". Failed to enable OCSP chaining. " +
    //                std::string(curl_easy_strerror(result)));
    //          }
  }
  m_allowFailedCrlRetrieval = options.SslOptions.AllowFailedCrlRetrieval;
#endif
  m_enableCrlValidation = options.SslOptions.EnableCertificateRevocationListCheck;

  if (!options.SslVerifyPeer)
  {
    if (!SetLibcurlOption(m_handle, CURLOPT_SSL_VERIFYPEER, 0L, &result))
    {
      throw Azure::Core::Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to disable ssl verify peer. " + std::string(curl_easy_strerror(result)));
    }
  }

  if (options.NoSignal)
  {
    if (!SetLibcurlOption(m_handle, CURLOPT_NOSIGNAL, 1L, &result))
    {
      throw Azure::Core::Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
          + ". Failed to set NOSIGNAL option for libcurl. "
          + std::string(curl_easy_strerror(result)));
    }
  }

  // curl-transport adapter supports only HTTP/1.1
  // https://github.com/Azure/azure-sdk-for-cpp/issues/2848
  // The libcurl uses HTTP/2 by default, if it can be negotiated with a server on handshake.
  if (!SetLibcurlOption(m_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1, &result))
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
        + ". Failed to set libcurl HTTP/1.1" + ". " + std::string(curl_easy_strerror(result)));
  }

  //   Make libcurl to support only TLS v1.2 or later
  if (!SetLibcurlOption(m_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2, &result))
  {
    throw Azure::Core::Http::TransportException(
        _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName
        + ". Failed enforcing TLS v1.2 or greater. " + std::string(curl_easy_strerror(result)));
  }

  auto performResult = curl_easy_perform(m_handle.get());
  if (performResult != CURLE_OK)
  {
#if defined(AZ_PLATFORM_LINUX)
    if (performResult == CURLE_PEER_FAILED_VERIFICATION)
    {
      curl_easy_getinfo(m_handle.get(), CURLINFO_SSL_VERIFYRESULT, &result);
      throw Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName + ". "
          + std::string(curl_easy_strerror(performResult))
          + ". Underlying error: " + X509_verify_cert_error_string(result));
    }
    else
#endif
    {
      throw Http::TransportException(
          _detail::DefaultFailedToGetNewConnectionTemplate + hostDisplayName + ". "
          + std::string(curl_easy_strerror(performResult)));
    }
  }

  //   Get the socket that libcurl is using from handle. Will use this to wait while
  // reading/writing
  // into wire
#if defined(_MSC_VER)
#pragma warning(push)
// C26812: The enum type 'CURLcode' is un-scoped. Prefer 'enum class' over 'enum' (Enum.3)
#pragma warning(disable : 26812)
#endif
  result = curl_easy_getinfo(m_handle.get(), CURLINFO_ACTIVESOCKET, &m_curlSocket);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  if (result != CURLE_OK)
  {
    throw Http::TransportException(
        "Broken connection. Couldn't get the active sockect for it."
        + std::string(curl_easy_strerror(result)));
  }
}
