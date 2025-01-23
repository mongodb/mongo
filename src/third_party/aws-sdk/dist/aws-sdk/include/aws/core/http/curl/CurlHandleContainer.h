/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/ResourceManager.h>
#include <aws/core/http/Version.h>

#include <utility>
#include <curl/curl.h>

namespace Aws
{
namespace Http
{

/**
  * Simple Connection pool manager for Curl. It maintains connections in a thread safe manner. You
  * can call into acquire a handle, then put it back when finished. It is assumed that reusing an already
  * initialized handle is preferable (especially for synchronous clients). The pool doubles in capacity as
  * needed up to the maximum amount of connections.
  */
class CurlHandleContainer
{
public:
    /**
      * Initializes an empty stack of CURL handles. If you are only making synchronous calls via your http client
      * then a small size is best. For async support, a good value would be 6 * number of Processors.   *
      */
    CurlHandleContainer(unsigned maxSize = 50, long httpRequestTimeout = 0, long connectTimeout = 1000, bool tcpKeepAlive = true,
                        unsigned long tcpKeepAliveIntervalMs = 30000, long lowSpeedTime = 3000, unsigned long lowSpeedLimit = 1,
                        Version version = Version::HTTP_VERSION_2TLS);
    ~CurlHandleContainer();

    /**
      * Blocks until a curl handle from the pool is available for use.
      */
    CURL* AcquireCurlHandle();
    /**
      * Returns a handle to the pool for reuse. It is imperative that this is called
      * after you are finished with the handle.
      */
    void ReleaseCurlHandle(CURL* handle);

    /**
     * When the handle has bad DNS entries, problematic live connections, we need to destroy the handle from pool.
     */
    void DestroyCurlHandle(CURL* handle);

private:
    CurlHandleContainer(const CurlHandleContainer&) = delete;
    const CurlHandleContainer& operator = (const CurlHandleContainer&) = delete;
    CurlHandleContainer(const CurlHandleContainer&&) = delete;
    const CurlHandleContainer& operator = (const CurlHandleContainer&&) = delete;

    CURL* CreateCurlHandleInPool();
    bool CheckAndGrowPool();
    void SetDefaultOptionsOnHandle(CURL* handle);
    static long ConvertHttpVersion(Version version);

    Aws::Utils::ExclusiveOwnershipResourceManager<CURL*> m_handleContainer;
    unsigned m_maxPoolSize;
    unsigned long m_httpRequestTimeout;
    unsigned long m_connectTimeout;
    bool m_enableTcpKeepAlive;
    unsigned long m_tcpKeepAliveIntervalMs;
    unsigned long m_lowSpeedTime;
    unsigned long m_lowSpeedLimit;
    unsigned m_poolSize;
    std::mutex m_containerLock;
    Version m_version;
};

} // namespace Http
} // namespace Aws

