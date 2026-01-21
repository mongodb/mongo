// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief The libcurl connection keeps the curl handle and performs the data transfer to the
 * network.
 */

#pragma once

#include "azure/core/http/http.hpp"
#include "azure/core/internal/unique_handle.hpp"

#include <chrono>
#include <string>

#if defined(_MSC_VER)
// C6101 : Returning uninitialized memory '*Mtu'->libcurl calling WSAGetIPUserMtu from WS2tcpip.h
#pragma warning(push)
#pragma warning(disable : 6101)
#endif

#include <curl/curl.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// From openssl/x509.h.  Avoids needing to include openssl headers
typedef struct x509_store_ctx_st X509_STORE_CTX;

namespace Azure { namespace Core {
  namespace _detail {
    /**
     * @brief  Unique handle for WinHTTP HINTERNET handles.
     *
     * @note HINTERNET is declared as a "void *". This means that this definition subsumes all other
     * `void *` types when used with Azure::Core::_internal::UniqueHandle.
     *
     */
    template <> struct UniqueHandleHelper<CURL>
    {
      using type = _internal::BasicUniqueHandle<CURL, curl_easy_cleanup>;
    };
  } // namespace _detail

  namespace Http {
    namespace _detail {
      // libcurl CURL_MAX_WRITE_SIZE is 64k. Using same value for default uploading chunk size.
      // This can be customizable in the HttpRequest
      constexpr static size_t DefaultUploadChunkSize = 1024 * 64;
      constexpr static size_t DefaultLibcurlReaderSize = 4 * 1024;
      // Run time error template
      constexpr static const char* DefaultFailedToGetNewConnectionTemplate
          = "Fail to get a new connection for: ";
      constexpr static int32_t DefaultMaxOpenNewConnectionIntentsAllowed = 10;
      // After 3 connections are received from the pool and failed to send a request, the next
      // connections would ask the pool to be clean and spawn new connection.
      constexpr static int32_t RequestPoolResetAfterConnectionFailed = 3;
      // 90 sec -> cleaner wait time before next clean routine
      constexpr static int32_t DefaultCleanerIntervalMilliseconds = 1000 * 90;
      // 60 sec -> expired connection is when it waits for 60 sec or more and it's not re-used
      constexpr static int32_t DefaultConnectionExpiredMilliseconds = 1000 * 60;
      // Define the maximun allowed connections per host-index in the pool. If this number is
      // reached for the host-index, next connections trying to be added to the pool will be
      // ignored.
      constexpr static int32_t MaxConnectionsPerIndex = 1024;

    } // namespace _detail

    /**
     * @brief Interface for the connection to the network with Curl.
     *
     * @remark This interface enables to mock the communication to the network with any behavior for
     * testing.
     *
     */
    class CurlNetworkConnection {
    private:
      bool m_isShutDown = false;

    public:
      /**
       * @brief Allow derived classes calling a destructor.
       *
       */
      virtual ~CurlNetworkConnection() = default;

      /**
       * @brief Get the Connection Properties Key object
       *
       */
      virtual std::string const& GetConnectionKey() const = 0;

      /**
       * @brief Update last usage time for the connection.
       *
       */
      virtual void UpdateLastUsageTime() = 0;

      /**
       * @brief Checks whether this CURL connection is expired.
       *
       */
      virtual bool IsExpired() = 0;

      /**
       * @brief This function is used when working with streams to pull more data from the wire.
       * Function will try to keep pulling data from socket until the buffer is all written or until
       * there is no more data to get from the socket.
       *
       */
      virtual size_t ReadFromSocket(uint8_t* buffer, size_t bufferSize, Context const& context) = 0;

      /**
       * @brief This method will use libcurl socket to write all the bytes from buffer.
       *
       */
      virtual CURLcode SendBuffer(uint8_t const* buffer, size_t bufferSize, Context const& context)
          = 0;

      /**
       * @brief Set the connection into an invalid and unusable state.
       *
       * @remark A connection won't be returned to the connection pool if it was shut it down.
       *
       */
      virtual void Shutdown() { m_isShutDown = true; }

      /**
       * @brief Check if the the connection was shut it down.
       *
       * @return `true` is the connection was shut it down; otherwise, `false`.
       */
      bool IsShutdown() const { return m_isShutDown; }
    };

    /**
     * @brief CURL HTTP connection.
     *
     */
    class CurlConnection final : public CurlNetworkConnection {
    private:
      Azure::Core::_internal::UniqueHandle<CURL> m_handle;
      curl_socket_t m_curlSocket;
      std::chrono::steady_clock::time_point m_lastUseTime;
      std::string m_connectionKey;
      // CRL validation is disabled by default to be consistent with WinHTTP behavior
      bool m_enableCrlValidation{false};
      // Allow the connection to proceed if retrieving the CRL failed.
      bool m_allowFailedCrlRetrieval{true};

      static int CurlLoggingCallback(
          CURL* handle,
          curl_infotype type,
          char* data,
          size_t size,
          void* userp);

      static int CurlSslCtxCallback(CURL* curl, void* sslctx, void* parm);
      int SslCtxCallback(CURL* curl, void* sslctx);
      int VerifyCertificateError(int ok, X509_STORE_CTX* storeContext);

    public:
      /**
       * @brief Construct CURL HTTP connection.
       *
       * @param request Remote request
       * @param options Connection options.
       * @param hostDisplayName Display name for remote host, used for diagnostics.
       *
       * @param connectionPropertiesKey CURL connection properties key
       */
      CurlConnection(
          Azure::Core::Http::Request& request,
          Azure::Core::Http::CurlTransportOptions const& options,
          std::string const& hostDisplayName,
          std::string const& connectionPropertiesKey);

      /**
       * @brief Destructor.
       * @details Cleans up CURL (invokes `curl_easy_cleanup()`).
       */
      ~CurlConnection() override {}

      std::string const& GetConnectionKey() const override { return this->m_connectionKey; }

      /**
       * @brief Update last usage time for the connection.
       *
       */
      void UpdateLastUsageTime() override
      {
        this->m_lastUseTime = std::chrono::steady_clock::now();
      }

      /**
       * @brief Checks whether this CURL connection is expired.
       * @return `true` if this connection is considered expired; otherwise, `false`.
       */
      bool IsExpired() override
      {
        auto connectionOnWaitingTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - this->m_lastUseTime);
        return connectionOnWaitingTimeMs.count() >= _detail::DefaultConnectionExpiredMilliseconds;
      }

      /**
       * @brief This function is used when working with streams to pull more data from the wire.
       * Function will try to keep pulling data from socket until the buffer is all written or until
       * there is no more data to get from the socket.
       *
       * @param context A context to control the request lifetime.
       * @param buffer ptr to buffer where to copy bytes from socket.
       * @param bufferSize size of the buffer and the requested bytes to be pulled from wire.
       * @return return the numbers of bytes pulled from socket. It can be less than what it was
       * requested.
       */
      size_t ReadFromSocket(uint8_t* buffer, size_t bufferSize, Context const& context) override;

      /**
       * @brief This method will use libcurl socket to write all the bytes from buffer.
       *
       * @remarks Hardcoded timeout is used in case a socket stop responding.
       *
       * @param context A context to control the request lifetime.
       * @param buffer ptr to the data to be sent to wire.
       * @param bufferSize size of the buffer to send.
       * @return CURL_OK when response is sent successfully.
       */
      CURLcode SendBuffer(uint8_t const* buffer, size_t bufferSize, Context const& context)
          override;
    };
  } // namespace Http
}} // namespace Azure::Core
