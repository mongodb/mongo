// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief The curl connection pool provides the utilities for creating a new curl connection and to
 * keep a pool of connections to be re-used.
 */

#pragma once

#include "azure/core/dll_import_export.hpp"
#include "azure/core/http/http.hpp"
#include "curl_connection_private.hpp"

#include <azure/core/http/curl_transport.hpp>

#include <atomic>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(_azure_TESTING_BUILD)
// Define the class name that reads from ConnectionPool private members
namespace Azure { namespace Core { namespace Test {
  class CurlConnectionPool_connectionPoolTest_Test;
  class CurlConnectionPool_uniquePort_Test;
  class CurlConnectionPool_connectionClose_Test;
  class SdkWithLibcurl_globalCleanUp_Test;
}}} // namespace Azure::Core::Test
#endif

namespace Azure { namespace Core { namespace Http { namespace _detail {

  /**
   * @brief CURL HTTP connection pool makes it possible to re-use one curl connection to perform
   * more than one request. Use this component when connections are not re-used by default.
   *
   * This pool offers static methods and it is allocated statically. There can be only one
   * connection pool per application.
   */
  class CurlConnectionPool final {
#if defined(_azure_TESTING_BUILD)
    // Give access to private to this tests class
    friend class Azure::Core::Test::CurlConnectionPool_connectionPoolTest_Test;
    friend class Azure::Core::Test::CurlConnectionPool_uniquePort_Test;
    friend class Azure::Core::Test::CurlConnectionPool_connectionClose_Test;
    friend class Azure::Core::Test::SdkWithLibcurl_globalCleanUp_Test;
#endif

  public:
    ~CurlConnectionPool()
    {
      using namespace Azure::Core::Http::_detail;
      if (m_cleanThread.joinable())
      {
        {
          std::unique_lock<std::mutex> lock(ConnectionPoolMutex);
          // Remove all connections
          g_curlConnectionPool.ConnectionPoolIndex.clear();
        }
        // Signal clean thread to wake up
        ConditionalVariableForCleanThread.notify_one();
        // join thread
        m_cleanThread.join();
      }
      curl_global_cleanup();
    }

    /**
     * @brief Finds a connection to be re-used from the connection pool.
     * @remark If there is not any available connection, a new connection is created.
     *
     * @param request HTTP request to get #Azure::Core::Http::CurlNetworkConnection for.
     * @param options The connection settings which includes host name and libcurl handle specific
     * configuration.
     * @param resetPool Request the pool to remove all current connections for the provided
     * options to force the creation of a new connection.
     *
     * @return #Azure::Core::Http::CurlNetworkConnection to use.
     */
    std::unique_ptr<CurlNetworkConnection> ExtractOrCreateCurlConnection(
        Request& request,
        CurlTransportOptions const& options,
        bool resetPool = false);

    /**
     * @brief Moves a connection back to the pool to be re-used.
     *
     * @param connection CURL HTTP connection to add to the pool.
     * @param httpKeepAlive The status of keep-alive behavior, based on HTTP protocol version and
     * the most recent response header received through the \p connection.
     */
    void MoveConnectionBackToPool(
        std::unique_ptr<CurlNetworkConnection> connection,
        bool httpKeepAlive);

    /**
     * @brief Keeps a unique key for each host and creates a connection pool for each key.
     *
     * @details This way getting a connection for a specific host can be done in O(1) instead of
     * looping a single connection list to find the first connection for the required host.
     *
     * @remark There might be multiple connections for each host.
     */
    std::unordered_map<std::string, std::list<std::unique_ptr<CurlNetworkConnection>>>
        ConnectionPoolIndex;

    std::mutex ConnectionPoolMutex;

    // This is used to put the cleaning pool thread to sleep and yet to be able to wake it if the
    // application finishes.
    std::condition_variable ConditionalVariableForCleanThread;

    AZ_CORE_DLLEXPORT static Azure::Core::Http::_detail::CurlConnectionPool g_curlConnectionPool;

    bool IsCleanThreadRunning = false;

  private:
    // private constructor to keep this as singleton.
    CurlConnectionPool() { curl_global_init(CURL_GLOBAL_ALL); }

    // Makes possible to know the number of current connections in the connection pool for an
    // index
    size_t ConnectionsOnPool(std::string const& host) { return ConnectionPoolIndex[host].size(); }

    std::thread m_cleanThread;
  };

}}}} // namespace Azure::Core::Http::_detail
