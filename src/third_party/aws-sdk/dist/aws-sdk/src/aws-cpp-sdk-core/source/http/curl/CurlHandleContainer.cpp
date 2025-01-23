/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/curl/CurlHandleContainer.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <algorithm>

using namespace Aws::Utils::Logging;
using namespace Aws::Http;

static const char* CURL_HANDLE_CONTAINER_TAG = "CurlHandleContainer";


CurlHandleContainer::CurlHandleContainer(unsigned maxSize, long httpRequestTimeout, long connectTimeout, bool enableTcpKeepAlive,
                                        unsigned long tcpKeepAliveIntervalMs, long lowSpeedTime, unsigned long lowSpeedLimit,
                                        Version version) :
                m_maxPoolSize(maxSize), m_httpRequestTimeout(httpRequestTimeout), m_connectTimeout(connectTimeout), m_enableTcpKeepAlive(enableTcpKeepAlive),
                m_tcpKeepAliveIntervalMs(tcpKeepAliveIntervalMs), m_lowSpeedTime(lowSpeedTime), m_lowSpeedLimit(lowSpeedLimit), m_poolSize(0),
                m_version(version)
{
    AWS_LOGSTREAM_INFO(CURL_HANDLE_CONTAINER_TAG, "Initializing CurlHandleContainer with size " << maxSize);
}

CurlHandleContainer::~CurlHandleContainer()
{
    AWS_LOGSTREAM_INFO(CURL_HANDLE_CONTAINER_TAG, "Cleaning up CurlHandleContainer.");
    for (CURL* handle : m_handleContainer.ShutdownAndWait(m_poolSize))
    {
        AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Cleaning up " << handle);
        curl_easy_cleanup(handle);
    }
    m_poolSize = 0;
}

CURL* CurlHandleContainer::AcquireCurlHandle()
{
    AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Attempting to acquire curl connection.");

    if(!m_handleContainer.HasResourcesAvailable())
    {
        AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "No current connections available in pool. Attempting to create new connections.");
        CheckAndGrowPool();
    }

    // TODO: 1.12: start to fail instead of infinite loop, possibly introduce another timeout config field
    CURL* handle = nullptr;
    bool errorLogged = false;  // avoid log explosion on legacy app behavior
    while (!handle) {
      constexpr unsigned long ACQUIRE_TIMEOUT = 1000l;  // some big enough arbitrary value, possibly need a user config or just fail ASAP.
      handle = m_handleContainer.TryAcquire(ACQUIRE_TIMEOUT);
      if (!handle && !errorLogged) {
        AWS_LOGSTREAM_ERROR(CURL_HANDLE_CONTAINER_TAG,
                            "Unable to Acquire a curl handle within 1 second. "
                            "Waiting further, this method will start failing in 1.12.x. "
                            "Please increase the pool size.");
        errorLogged = true;
      }
    }
    AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Connection has been released. Continuing.");
    AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Returning connection handle " << handle);
    return handle;
}

void CurlHandleContainer::ReleaseCurlHandle(CURL* handle)
{
    if (handle)
    {
#if LIBCURL_VERSION_NUM >= 0x074D00 // 7.77.0
        curl_easy_setopt(handle, CURLOPT_COOKIEFILE, NULL); // workaround a mem leak on curl
#endif
        curl_easy_reset(handle);
        SetDefaultOptionsOnHandle(handle);
        AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Releasing curl handle " << handle);
        m_handleContainer.Release(handle);
        AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Notified waiting threads.");
    }
}

void CurlHandleContainer::DestroyCurlHandle(CURL* handle)
{
    if (!handle)
    {
        return;
    }

    curl_easy_cleanup(handle);
    AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Destroy curl handle: " << handle);
    {
        std::lock_guard<std::mutex> locker(m_containerLock);
        // Other threads could be blocked and waiting on m_handleContainer.Acquire()
        // If the handle is not released back to the pool, it could create a deadlock
        // Create a new handle and release that into the pool
        handle = CreateCurlHandleInPool();
        if (!handle && m_poolSize) {
          m_poolSize -= 1;
        }
    }
    if (handle)
    {
        AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "Created replacement handle and released to pool: " << handle);
    } else {
      AWS_LOGSTREAM_ERROR(CURL_HANDLE_CONTAINER_TAG,
                          "Failed to create a replacement handle. The handle pool size reduced to " << m_poolSize);
    }
}


CURL* CurlHandleContainer::CreateCurlHandleInPool()
{
    CURL* curlHandle = curl_easy_init();

    if (curlHandle)
    {
        SetDefaultOptionsOnHandle(curlHandle);
        m_handleContainer.Release(curlHandle);
    }
    else
    {
        AWS_LOGSTREAM_ERROR(CURL_HANDLE_CONTAINER_TAG, "curl_easy_init failed to allocate.");
    }
    return curlHandle;
}

bool CurlHandleContainer::CheckAndGrowPool()
{
    std::lock_guard<std::mutex> locker(m_containerLock);
    if (m_poolSize < m_maxPoolSize)
    {
        unsigned multiplier = m_poolSize > 0 ? m_poolSize : 1;
        unsigned amountToAdd = (std::min)(multiplier * 2, m_maxPoolSize - m_poolSize);
        AWS_LOGSTREAM_DEBUG(CURL_HANDLE_CONTAINER_TAG, "attempting to grow pool size by " << amountToAdd);

        unsigned actuallyAdded = 0;
        for (unsigned i = 0; i < amountToAdd; ++i)
        {
            CURL* curlHandle = CreateCurlHandleInPool();

            if (curlHandle)
            {
                ++actuallyAdded;
            }
            else
            {
                break;
            }
        }

        AWS_LOGSTREAM_INFO(CURL_HANDLE_CONTAINER_TAG, "Pool grown by " << actuallyAdded);
        m_poolSize += actuallyAdded;

        return actuallyAdded > 0;
    }

    AWS_LOGSTREAM_INFO(CURL_HANDLE_CONTAINER_TAG, "Pool cannot be grown any further, already at max size.");

    return false;
}

void CurlHandleContainer::SetDefaultOptionsOnHandle(CURL* handle)
{
    //for timeouts to work in a multi-threaded context,
    //always turn signals off. This also forces dns queries to
    //not be included in the timeout calculations.
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, m_httpRequestTimeout);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, m_connectTimeout);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, m_lowSpeedLimit);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, m_lowSpeedTime < 1000 ? (m_lowSpeedTime == 0 ? 0 : 1) : m_lowSpeedTime / 1000);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, m_enableTcpKeepAlive ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, m_tcpKeepAliveIntervalMs / 1000);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, m_tcpKeepAliveIntervalMs / 1000);
    curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, ConvertHttpVersion(m_version));
    curl_easy_setopt(handle, CURLOPT_MAXCONNECTS, m_maxPoolSize);
}

long CurlHandleContainer::ConvertHttpVersion(Version version) {
    if (version == Version::HTTP_VERSION_NONE)
    {
        return CURL_HTTP_VERSION_NONE;
    }
    else if (version == Version::HTTP_VERSION_1_0)
    {
        return CURL_HTTP_VERSION_1_0;
    }
    else if (version == Version::HTTP_VERSION_1_1)
    {
        return CURL_HTTP_VERSION_1_1;
    }
#if LIBCURL_VERSION_NUM >= 0x072100 // 7.33.0
    else if (version == Version::HTTP_VERSION_2_0)
    {
        return CURL_HTTP_VERSION_2_0;
    }
#endif
#if LIBCURL_VERSION_NUM >= 0x072F00 // 7.47.0
    else if (version == Version::HTTP_VERSION_2TLS)
    {
        return CURL_HTTP_VERSION_2TLS;
    }
#endif
#if LIBCURL_VERSION_NUM >= 0x073100 // 7.49.0
    else if (version == Version::HTTP_VERSION_2_PRIOR_KNOWLEDGE)
    {
        return CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE;
    }
#endif
#if LIBCURL_VERSION_NUM >= 0x074200 // 7.66.0
    else if (version == Version::HTTP_VERSION_3)
    {
        return CURL_HTTP_VERSION_3;
    }
#endif
#if LIBCURL_VERSION_NUM >= 0x075800 // 7.88.0
    else if (version == Version::HTTP_VERSION_3ONLY)
    {
        return CURL_HTTP_VERSION_3ONLY;
    }
#endif
#if LIBCURL_VERSION_NUM >= 0x073E00 // 7.62.0
    return CURL_HTTP_VERSION_2TLS;
#else
    return CURL_HTTP_VERSION_1_1;
#endif
}
