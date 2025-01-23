/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/crt/CRTHttpClient.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/utils/ratelimiter/RateLimiterInterface.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/crypto/Hash.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <aws/crt/http/HttpConnectionManager.h>
#include <aws/crt/http/HttpRequestResponse.h>

static const char *const CRT_HTTP_CLIENT_TAG = "CRTHttpClient";

// Adapts AWS SDK input streams and rate limiters to the CRT input stream reading model.
class SDKAdaptingInputStream : public Aws::Crt::Io::StdIOStreamInputStream {
public:
    SDKAdaptingInputStream(const std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface>& rateLimiter,
                           std::shared_ptr<Aws::Crt::Io::IStream> stream,
                           const Aws::Http::HttpClient& client,
                           const Aws::Http::HttpRequest& request,
                           bool isStreaming,
                           Aws::Crt::Allocator* allocator = Aws::Crt::ApiAllocator()) noexcept :
        Aws::Crt::Io::StdIOStreamInputStream(std::move(stream), allocator),
        m_rateLimiter(rateLimiter),
        m_client(client),
        m_currentRequest(request),
        m_isStreaming(isStreaming),
        m_chunkEnd(false)
    {
    }

protected:

    bool ReadImpl(Aws::Crt::ByteBuf &buffer) noexcept override
    {
        if (!m_client.ContinueRequest(m_currentRequest) || !m_client.IsRequestProcessingEnabled())
        {
            return false;
        }

        bool isAwsChunked = m_currentRequest.HasHeader(Aws::Http::CONTENT_ENCODING_HEADER) &&
            m_currentRequest.GetHeaderValue(Aws::Http::CONTENT_ENCODING_HEADER) == Aws::Http::AWS_CHUNKED_VALUE;

        size_t amountToRead = buffer.capacity - buffer.len;
        uint8_t* originalBufferPos = buffer.buffer;

        // aws-chunk = hex(chunk-size) + CRLF + chunk-data + CRLF
        // Needs to reserve bytes of sizeof(hex(chunk-size)) + sizeof(CRLF) + sizeof(CRLF)
        if (isAwsChunked)
        {
            Aws::String amountToReadHexString = Aws::Utils::StringUtils::ToHexString(amountToRead);
            amountToRead -= (amountToReadHexString.size() + 4);
        }        

        // initial check to see if we should avoid reading for the moment.
        if (!m_rateLimiter || (m_rateLimiter && m_rateLimiter->ApplyCost(0) == std::chrono::milliseconds(0))) {
            size_t currentPos = buffer.len;

            // now do the read. We may over read by an IO buffer size, but it's fine. The throttle will still
            // kick-in in plenty of time.
            bool retValue = false;
            if (!m_isStreaming)
            {
                retValue = Aws::Crt::Io::StdIOStreamInputStream::ReadImpl(buffer);
            }
            else
            {
                if (StdIOStreamInputStream::GetStatusImpl().is_valid && StdIOStreamInputStream::PeekImpl() != std::char_traits<char>::eof())
                {
                    retValue = Aws::Crt::Io::StdIOStreamInputStream::ReadSomeImpl(buffer);
                }
            }
            size_t newPos = buffer.len;
            AWS_ASSERT(newPos >= currentPos && "the buffer length should not have decreased in value.");

            if (retValue && m_isStreaming)
            {
                Aws::Crt::Io::StreamStatus streamStatus;
                GetStatus(streamStatus);

                if (newPos == currentPos && !streamStatus.is_end_of_stream && streamStatus.is_valid)
                {
                    return true;
                }
            }
            
            size_t amountRead = newPos - currentPos;

            if (isAwsChunked)
            {
                // if we have a chunk to wrap, wrap it, be sure to update the running checksum.
                if (amountRead > 0)
                {
                    if (m_currentRequest.GetRequestHash().second != nullptr)
                    {
                        m_currentRequest.GetRequestHash().second->Update(reinterpret_cast<unsigned char*>(originalBufferPos), amountRead);
                    }

                    Aws::String hex = Aws::Utils::StringUtils::ToHexString(amountRead);
                    // this is safe because of the isAwsChunked branch above.
                    // I don't see a aws_byte_buf equivalent of memmove. This is lifted from the curl implementation.
                    memmove(originalBufferPos + hex.size() + 2, originalBufferPos, amountRead);
                    memmove(originalBufferPos + hex.size() + 2 + amountRead, "\r\n", 2);
                    memmove(originalBufferPos, hex.c_str(), hex.size());
                    memmove(originalBufferPos + hex.size(), "\r\n", 2);
                    amountRead += hex.size() + 4;
                }
                else if (!m_chunkEnd)
                {
                    // if we didn't read anything, then lets finish up the chunk and send it.
                    // the reference implementation seems to assume only one chunk is allowed, because the chunkEnd bit is never updated.
                    // keep that same behavior here.
                    Aws::StringStream chunkedTrailer;
                    chunkedTrailer << "0\r\n";
                    if (m_currentRequest.GetRequestHash().second != nullptr)
                    {
                        chunkedTrailer << "x-amz-checksum-" << m_currentRequest.GetRequestHash().first << ":"
                            << Aws::Utils::HashingUtils::Base64Encode(m_currentRequest.GetRequestHash().second->GetHash().GetResult()) << "\r\n";
                    }
                    chunkedTrailer << "\r\n";
                    amountRead = chunkedTrailer.str().size();
                    memcpy(originalBufferPos, chunkedTrailer.str().c_str(), amountRead);
                    m_chunkEnd = true;
                }
                buffer.len += amountRead;
            }

            auto& sentHandler = m_currentRequest.GetDataSentEventHandler();
            if (sentHandler)
            {
                sentHandler(&m_currentRequest, static_cast<long long>(amountRead));
            }

            if (m_rateLimiter)
            {
                // now actually reduce the window.
                m_rateLimiter->ApplyCost(static_cast<int64_t>(newPos - currentPos));
                return retValue;
            }
        }

        return true;
    }

private:
    std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface> m_rateLimiter;
    const Aws::Http::HttpClient& m_client;
    const Aws::Http::HttpRequest& m_currentRequest;
    bool m_isStreaming;
    bool m_chunkEnd;
};

// Just a wrapper around a Condition Variable and a mutex, which handles wait and timed waits while protecting
// from spurious wakeups.
class AsyncWaiter
{
public:
    AsyncWaiter() = default;
    AsyncWaiter(const AsyncWaiter&) = delete;
    AsyncWaiter& operator=(const AsyncWaiter&) = delete;

    void Wakeup()
    {
        {
            std::lock_guard<std::mutex> locker(m_lock);
            m_wakeupIntentional = true;
        }
        m_cvar.notify_one();
    }

    void WaitOnCompletion()
    {
        std::unique_lock<std::mutex> uniqueLocker(m_lock);
        m_cvar.wait(uniqueLocker, [this](){return m_wakeupIntentional;});
    }

    bool WaitOnCompletionFor(const size_t ms)
    {
        std::unique_lock<std::mutex> uniqueLocker(m_lock);
        return m_cvar.wait_for(uniqueLocker, std::chrono::milliseconds(ms), [this](){return m_wakeupIntentional;});
    }

private:
    std::mutex m_lock;
    std::condition_variable m_cvar;
    bool m_wakeupIntentional{false};
};

namespace Aws
{
    namespace Http
    {
        CRTHttpClient::CRTHttpClient(const Aws::Client::ClientConfiguration& clientConfig, Crt::Io::ClientBootstrap& bootstrap) : 
            HttpClient(), m_context(), m_proxyOptions(), m_bootstrap(bootstrap), m_configuration(clientConfig)
        {
            //first need to figure TLS out...
            Crt::Io::TlsContextOptions tlsContextOptions = Crt::Io::TlsContextOptions::InitDefaultClient();
            CheckAndInitializeProxySettings(clientConfig);

            // Given current SDK configuration assumptions, if the ca is overridden and a proxy is configured,
            // it's intended for the proxy, not this context.
            if (!m_proxyOptions.has_value())
            {
                if (!m_configuration.caPath.empty() || !m_configuration.caFile.empty())
                {
                    const char* caPath = m_configuration.caPath.empty() ? nullptr : m_configuration.caPath.c_str();
                    const char* caFile = m_configuration.caFile.empty() ? nullptr : m_configuration.caFile.c_str();
                    if (!tlsContextOptions.OverrideDefaultTrustStore(caPath, caFile))
                    {
                        m_bad = true;
                        return;
                    }
                }
            }

            tlsContextOptions.SetVerifyPeer(m_configuration.verifySSL);
            
            if (Crt::Io::TlsContextOptions::IsAlpnSupported())
            {
                // this may need to be pulled from the client configuration....
                if (!tlsContextOptions.SetAlpnList("h2;http/1.1"))
                {
                    m_bad = true;
                    return;
                }
            }

            Crt::Io::TlsContext newContext(tlsContextOptions, Crt::Io::TlsMode::CLIENT);

            if (!newContext)
            {
                m_bad = true;
                return;
            }

            m_context = std::move(newContext);
        }

        // this isn't entirely necessary, but if you want to be nice to debuggers and memory checkers, let's go ahead
        // and shut everything down cleanly.
        CRTHttpClient::~CRTHttpClient()
        {
            Aws::Vector<std::future<void>> shutdownFutures;

            for (auto& managerPair : m_connectionPools) 
            {
                shutdownFutures.push_back(managerPair.second->InitiateShutdown());
            }

            for (auto& shutdownFuture : shutdownFutures)
            {
                shutdownFuture.get();
            }

            shutdownFutures.clear();
            m_connectionPools.clear();
        }

        static void AddRequestMetadataToCrtRequest(const std::shared_ptr<HttpRequest>& request, const std::shared_ptr<Crt::Http::HttpRequest>& crtRequest)
        {
            const char* methodStr = Aws::Http::HttpMethodMapper::GetNameForHttpMethod(request->GetMethod());
            AWS_LOGSTREAM_TRACE(CRT_HTTP_CLIENT_TAG, "Making " << methodStr << " request to " << request->GetURIString());
            AWS_LOGSTREAM_TRACE(CRT_HTTP_CLIENT_TAG, "Including headers:");
            //Add http headers to the request.
            for (const auto& header : request->GetHeaders())
            {
                Crt::Http::HttpHeader crtHeader;
                AWS_LOGSTREAM_TRACE(CRT_HTTP_CLIENT_TAG, header.first << ": " << header.second);
                crtHeader.name = Crt::ByteCursorFromArray((const uint8_t *)header.first.data(), header.first.length());
                crtHeader.value = Crt::ByteCursorFromArray((const uint8_t *)header.second.data(), header.second.length());
                crtRequest->AddHeader(crtHeader);
            }

            // HTTP method, GET, PUT, DELETE, etc...
            auto methodCursor = Crt::ByteCursorFromCString(methodStr);
            crtRequest->SetMethod(methodCursor);

            // Path portion of the request
            auto pathStrCpy = request->GetUri().GetURLEncodedPathRFC3986();
            auto queryStrCpy = request->GetUri().GetQueryString();
            Aws::StringStream ss;

            //CRT client has you pass the query string as part of the path. concatenate that here.
            ss << pathStrCpy << queryStrCpy;
            auto fullPathAndQueryCpy = ss.str();
            auto pathCursor = Crt::ByteCursorFromArray((uint8_t *)fullPathAndQueryCpy.c_str(), fullPathAndQueryCpy.length());
            crtRequest->SetPath(pathCursor);
        }

        static void OnResponseBodyReceived(Crt::Http::HttpStream& stream, const Crt::ByteCursor& body, const std::shared_ptr<HttpResponse>& response, const std::shared_ptr<HttpRequest>& request, const Http::HttpClient& client)
        {
            if (!client.ContinueRequest(*request) || !client.IsRequestProcessingEnabled())
            {
                AWS_LOGSTREAM_INFO(CRT_HTTP_CLIENT_TAG, "Request canceled. Canceling request by closing the connection.");
                stream.GetConnection().Close();                
                return;
            }

            //TODO: handle the read rate limiter here, once back pressure is setup.
            for (const auto& hashIterator : request->GetResponseValidationHashes())
            {
                hashIterator.second->Update(reinterpret_cast<unsigned char*>(body.ptr), body.len);
            }

            // When data is received from the content body of the incoming response, just copy it to the output stream.
            assert(response);
            response->GetResponseBody().write((const char*)body.ptr, static_cast<long>(body.len));
            if (response->GetResponseBody().fail()) {
                const auto& ref = response->GetResponseBody();
                AWS_LOGSTREAM_ERROR(CRT_HTTP_CLIENT_TAG, "Failed to write " << body.len << " (eof: " << ref.eof() << ", bad: " << ref.bad() << ")");
            }

            if (request->IsEventStreamRequest() && !response->HasHeader(Aws::Http::X_AMZN_ERROR_TYPE))
            {
                response->GetResponseBody().flush();
            }

            auto& receivedHandler = request->GetDataReceivedEventHandler();
            if (receivedHandler)
            {
                receivedHandler(request.get(), response.get(), static_cast<long long>(body.len));
            }

            AWS_LOGSTREAM_TRACE(CRT_HTTP_CLIENT_TAG, body.len << " bytes written to response.");

        }

        // on response headers arriving, write them to the response.
        static void OnIncomingHeaders(Crt::Http::HttpStream&, enum aws_http_header_block block, const Crt::Http::HttpHeader* headersArray, std::size_t headersCount, const std::shared_ptr<HttpResponse>& response)
        {
            if (block == AWS_HTTP_HEADER_BLOCK_INFORMATIONAL) return;

            AWS_LOGSTREAM_TRACE(CRT_HTTP_CLIENT_TAG, "Received Headers: ");

            for (size_t i = 0; i < headersCount; ++i)
            {
                const Crt::Http::HttpHeader* header = &headersArray[i];
                Aws::String headerNameStr((const char*)header->name.ptr, header->name.len);
                Aws::String headerValueStr((const char*)header->value.ptr, header->value.len);
                AWS_LOGSTREAM_TRACE(CRT_HTTP_CLIENT_TAG, headerNameStr << ": " << headerValueStr);
                response->AddHeader(headerNameStr, std::move(headerValueStr));
            }
        }

        static void OnIncomingHeadersBlockDone(Crt::Http::HttpStream& stream, enum aws_http_header_block, const std::shared_ptr<HttpResponse>& response)
        {
            AWS_LOGSTREAM_TRACE(CRT_HTTP_CLIENT_TAG, "Received response code: " << stream.GetResponseStatusCode());
            response->SetResponseCode((HttpResponseCode)stream.GetResponseStatusCode());
        }

        // Request is done. If there was an error set it, otherwise just wake up the cvar.
        static void OnStreamComplete(Crt::Http::HttpStream&, int errorCode, AsyncWaiter& waiter, const std::shared_ptr<HttpResponse>& response)
        {
            if (errorCode)
            {
                //TODO: get the right error parsed out.
                response->SetClientErrorType(Aws::Client::CoreErrors::NETWORK_CONNECTION);
                response->SetClientErrorMessage(aws_error_debug_str(errorCode));
            }

            waiter.Wakeup();
        }

        // if the connection acquisition failed, go ahead and fail the request and wakeup the cvar.
        // If it succeeded go ahead and make the request.
        static void OnClientConnectionAvailable(std::shared_ptr<Crt::Http::HttpClientConnection> connection, int errorCode, std::shared_ptr<Crt::Http::HttpClientConnection>& connectionReference,
                                         Crt::Http::HttpRequestOptions& requestOptions, AsyncWaiter& waiter, const std::shared_ptr<HttpRequest>& request, 
                                         const std::shared_ptr<HttpResponse>& response, const HttpClient& client)
        {
            bool shouldContinueRequest = client.ContinueRequest(*request);

            if (!shouldContinueRequest)
            {
                response->SetClientErrorType(Client::CoreErrors::USER_CANCELLED);
                response->SetClientErrorMessage("Request cancelled by user's continuation handler");
                waiter.Wakeup();
                return;
            }

            int finalErrorCode = errorCode;
            if (connection)
            {
                AWS_LOGSTREAM_DEBUG(CRT_HTTP_CLIENT_TAG, "Obtained connection handle " << (void*)connection.get());

                auto clientStream = connection->NewClientStream(requestOptions);
                connectionReference = connection;

                if (clientStream && clientStream->Activate()) {
                    return;
                }

                finalErrorCode = aws_last_error();
                AWS_LOGSTREAM_ERROR(CRT_HTTP_CLIENT_TAG, "Initiation of request failed because " << aws_error_debug_str(finalErrorCode));

            }

            const char *errorMsg = aws_error_debug_str(finalErrorCode);
            AWS_LOGSTREAM_ERROR(CRT_HTTP_CLIENT_TAG, "Obtaining connection failed because " << errorMsg);
            response->SetClientErrorType(Aws::Client::CoreErrors::NETWORK_CONNECTION);
            response->SetClientErrorMessage(errorMsg);

            waiter.Wakeup();
        }

        std::shared_ptr<HttpResponse> CRTHttpClient::MakeRequest(const std::shared_ptr<HttpRequest>& request,
                                                                 Aws::Utils::RateLimits::RateLimiterInterface*,
                                                                 Aws::Utils::RateLimits::RateLimiterInterface*) const
        {
            auto crtRequest = Crt::MakeShared<Crt::Http::HttpRequest>(Crt::g_allocator);
            auto response = Aws::MakeShared<Standard::StandardHttpResponse>(CRT_HTTP_CLIENT_TAG, request);

            auto requestConnOptions = CreateConnectionOptionsForRequest(request);
            auto connectionManager = GetWithCreateConnectionManagerForRequest(request, requestConnOptions);

            if (!connectionManager)
            {
                response->SetClientErrorMessage(aws_error_debug_str(aws_last_error()));
                response->SetClientErrorType(Client::CoreErrors::INVALID_PARAMETER_COMBINATION);
                return response;
            }
            AddRequestMetadataToCrtRequest(request, crtRequest);

            // Set the request body stream on the crt request. Setup the write rate limiter if present
            if (request->GetContentBody())
            {
                bool isStreaming = request->IsEventStreamRequest();
                crtRequest->SetBody(Aws::MakeShared<SDKAdaptingInputStream>(CRT_HTTP_CLIENT_TAG, m_configuration.writeRateLimiter, request->GetContentBody(), *this, *request, isStreaming));
            }

            Crt::Http::HttpRequestOptions requestOptions;
            requestOptions.request = crtRequest.get();

            requestOptions.onIncomingBody =
                [this, request, response](Crt::Http::HttpStream& stream, const Crt::ByteCursor& body)
            {
                OnResponseBodyReceived(stream, body, response, request, *this);
            };

            requestOptions.onIncomingHeaders =
                [response](Crt::Http::HttpStream& stream, enum aws_http_header_block block, const Crt::Http::HttpHeader* headersArray, std::size_t headersCount)
            {
                OnIncomingHeaders(stream, block, headersArray, headersCount, response);
            };

            // This will arrive at or around the same time as the headers. Use it to set the response code on the response
            requestOptions.onIncomingHeadersBlockDone =
                [request, response](Crt::Http::HttpStream& stream, enum aws_http_header_block block)
            {
                OnIncomingHeadersBlockDone(stream, block, response);
                auto& headersHandler = request->GetHeadersReceivedEventHandler();
                if (headersHandler)
                {
                    headersHandler(request.get(), response.get());
                }
            };

            // CRT client is async only so we'll need to do the synchronous part ourselves.
            // We'll use a condition variable and wait on it until the request completes or errors out.
            AsyncWaiter waiter;

            requestOptions.onStreamComplete =
                [&waiter, &response](Crt::Http::HttpStream& stream, int errorCode)
            {
                OnStreamComplete(stream, errorCode, waiter, response);
            };

            std::shared_ptr<Crt::Http::HttpClientConnection> connectionRef(nullptr);

            // now we finally have the request, get a connection and make the request.
            connectionManager->AcquireConnection(
                    [&connectionRef, &requestOptions, response, &waiter, request, this]
                    (std::shared_ptr<Crt::Http::HttpClientConnection> connection, int errorCode)
                    {
                        OnClientConnectionAvailable(connection, errorCode, connectionRef, requestOptions, waiter, request, response, *this);
                    });

            bool waiterTimedOut = false;
            // Naive http request timeout implementation. This doesn't factor in how long it took to get the connection from the pool, and
            // I'm undecided on the queueing theory implications of this decision so if this turns out to be the wrong granularity
            // this is the section of code you should be changing. You can probably get "close" by having an additional
            // atomic (not necessarily full on atomics implementation, but it needs to be the size of a WORD if it's not)
            // counter that gets incremented in the acquireConnection callback as long as your connection timeout
            // is shorter than your request timeout. Even if it's not, that would handle like.... 4-5 nines of getting this right.
            // since in the worst case scenario, your connect timeout got preempted by the request timeout, and is it really worth
            // all that effort if that's the worst thing that can happen?
            if (m_configuration.requestTimeoutMs > 0 )
            {
                waiterTimedOut = !waiter.WaitOnCompletionFor(m_configuration.requestTimeoutMs);

                // if this is true, the waiter timed out without a terminal condition being woken up.
                if (waiterTimedOut)
                {
                    // close the connection if it's still there so we can expedite anything we're waiting on.
                    if (connectionRef)
                    {
                        connectionRef->Close();
                    }
                }
            }

            // always wait, even if the above section timed out, because Wakeup() hasn't yet been called,
            // and this means we're still waiting on some queued up callbacks to fire.
            // going past this point before that occurs will cause a segfault when the callback DOES finally fire
            // since the waiter is on the stack.
            waiter.WaitOnCompletion();

            // now handle if we timed out or not.
            if (waiterTimedOut)
            {
                response->SetClientErrorType(
                        Aws::Client::CoreErrors::REQUEST_TIMEOUT);
                response->SetClientErrorMessage("Request Timeout Has Expired");
            }

            // TODO: is VOX support still a thing? If so we need to add the metrics for it.
            return response;
        }

        Aws::String CRTHttpClient::ResolveConnectionPoolKey(const URI& uri)
        {
            // create a unique key for this endpoint.
            Aws::StringStream ss;
            ss << SchemeMapper::ToString(uri.GetScheme()) << "://" << uri.GetAuthority() << ":" << uri.GetPort();

            return ss.str();
        }

        // The main purpose of this is to ensure there's exactly one connection manager per unique endpoint.
        // To do so, we simply keep a hash table of the endpoint key (see ResolveConnectionPoolKey()), and
        // put a connection manager for that endpoint as the value.
        // This runs in multiple threads potentially so there's a lock around it.
        std::shared_ptr<Crt::Http::HttpClientConnectionManager> CRTHttpClient::GetWithCreateConnectionManagerForRequest(const std::shared_ptr<HttpRequest>& request, const Crt::Http::HttpClientConnectionOptions& options) const
        {
            const auto connManagerRequestKey = ResolveConnectionPoolKey(request->GetUri());

            std::lock_guard<std::mutex> locker(m_connectionPoolLock);

            const auto& foundManager = m_connectionPools.find(connManagerRequestKey);

            // We've already got one, return it.
            if (foundManager != m_connectionPools.cend()) {
                return foundManager->second;
            }

            // don't have a manager for this endpoint, so create one for it.
            Crt::Http::HttpClientConnectionManagerOptions connectionManagerOptions;
            connectionManagerOptions.ConnectionOptions = options;
            connectionManagerOptions.MaxConnections = m_configuration.maxConnections;
            connectionManagerOptions.EnableBlockingShutdown = true;
            //TODO: need to bind out Monitoring options to handle the read timeout config value.
            // once done, come back and use it to setup read timeouts.

            auto connectionManager = Crt::Http::HttpClientConnectionManager::NewClientConnectionManager(connectionManagerOptions);

            if (!connectionManager)
            {
                return nullptr;
            }

            // put it in the hash table and return it.
            m_connectionPools.emplace(connManagerRequestKey, connectionManager);

            return connectionManager;
        }

        Crt::Http::HttpClientConnectionOptions CRTHttpClient::CreateConnectionOptionsForRequest(const std::shared_ptr<HttpRequest>& request) const
        {
            // connection options are unique per request, this is mostly just connection-level configuration mapping.
            Crt::Http::HttpClientConnectionOptions connectionOptions;
            connectionOptions.HostName = request->GetUri().GetAuthority().c_str();
            // TODO: come back and update this when we hook up the rate limiters.
            connectionOptions.ManualWindowManagement = false;
            connectionOptions.Port = request->GetUri().GetPort();
            
            if (m_context.has_value() && request->GetUri().GetScheme() == Scheme::HTTPS) 
            {
                connectionOptions.TlsOptions = m_context.value().NewConnectionOptions();
                auto serverName = request->GetUri().GetAuthority();
                auto serverNameCursor = Crt::ByteCursorFromCString(serverName.c_str());
                connectionOptions.TlsOptions->SetServerName(serverNameCursor);
            }

            connectionOptions.Bootstrap = &m_bootstrap;
            
            if (m_proxyOptions.has_value())
            {
                connectionOptions.ProxyOptions = m_proxyOptions.value();
            }

            connectionOptions.SocketOptions.SetConnectTimeoutMs(m_configuration.connectTimeoutMs);
            connectionOptions.SocketOptions.SetKeepAlive(m_configuration.enableTcpKeepAlive);

            if (m_configuration.enableTcpKeepAlive)
            {
                connectionOptions.SocketOptions.SetKeepAliveIntervalSec(
                        (uint16_t) (m_configuration.tcpKeepAliveIntervalMs / 1000));
            }
            connectionOptions.SocketOptions.SetSocketType(Crt::Io::SocketType::Stream);

            return connectionOptions;
        }

        // The proxy config is pretty hefty, so we don't want to create one for each request when we don't have to.
        // This converts whatever proxy settings are in clientConfig to CRT specific proxy settings.
        // It then sets it on the member variable for re-use elsewhere.
        void CRTHttpClient::CheckAndInitializeProxySettings(const Aws::Client::ClientConfiguration& clientConfig)
        {
            if (!clientConfig.proxyHost.empty())
            {
                Crt::Http::HttpClientConnectionProxyOptions proxyOptions;

                if (!clientConfig.proxyUserName.empty())
                {
                    proxyOptions.AuthType = Crt::Http::AwsHttpProxyAuthenticationType::Basic;
                    proxyOptions.BasicAuthUsername = clientConfig.proxyUserName.c_str();
                    proxyOptions.BasicAuthPassword = clientConfig.proxyPassword.c_str();
                }

                proxyOptions.HostName = m_configuration.proxyHost.c_str();

                if (clientConfig.proxyPort != 0)
                {
                    proxyOptions.Port = static_cast<uint16_t>(clientConfig.proxyPort);
                }
                else
                {
                    proxyOptions.Port = clientConfig.proxyScheme == Scheme::HTTPS ? 443 : 80;
                }

                if (clientConfig.proxyScheme == Scheme::HTTPS)
                {
                    Crt::Io::TlsContextOptions contextOptions = Crt::Io::TlsContextOptions::InitDefaultClient();

                    if (clientConfig.proxySSLKeyPassword.empty() && !clientConfig.proxySSLCertPath.empty())
                    {
                        const char* certPath = clientConfig.proxySSLCertPath.empty() ? nullptr : clientConfig.proxySSLCertPath.c_str();
                        const char* certFile = clientConfig.proxySSLKeyPath.empty() ? nullptr : clientConfig.proxySSLKeyPath.c_str();
                        contextOptions = Crt::Io::TlsContextOptions::InitClientWithMtls(certPath, certFile);
                    }
                    else if (!clientConfig.proxySSLKeyPassword.empty())
                    {
                        const char* pkcs12CertFile = clientConfig.proxySSLKeyPath.empty() ? nullptr : clientConfig.proxySSLKeyPath.c_str();
                        const char* pkcs12Pwd = clientConfig.proxySSLKeyPassword.c_str();
                        contextOptions = Crt::Io::TlsContextOptions::InitClientWithMtlsPkcs12(pkcs12CertFile, pkcs12Pwd);
                    }

                    if (!m_configuration.proxyCaFile.empty() || !m_configuration.proxyCaPath.empty()) 
                    {
                        const char* caPath = clientConfig.proxyCaPath.empty() ? nullptr : clientConfig.proxyCaPath.c_str();
                        const char* caFile = clientConfig.proxyCaFile.empty() ? nullptr : clientConfig.proxyCaFile.c_str();
                        contextOptions.OverrideDefaultTrustStore(caPath, caFile);
                    } 
                    else if (!m_configuration.caFile.empty() || !m_configuration.caPath.empty())
                    {
                        const char* caPath = clientConfig.caPath.empty() ? nullptr : clientConfig.caPath.c_str();
                        const char* caFile = clientConfig.caFile.empty() ? nullptr : clientConfig.caFile.c_str();
                        contextOptions.OverrideDefaultTrustStore(caPath, caFile);
                    }

                    contextOptions.SetVerifyPeer(clientConfig.verifySSL);
                    Crt::Io::TlsContext context = Crt::Io::TlsContext(contextOptions, Crt::Io::TlsMode::CLIENT);
                    proxyOptions.TlsOptions = context.NewConnectionOptions();                    
                }

                m_proxyOptions = std::move(proxyOptions);
            }
        }

    }
}
