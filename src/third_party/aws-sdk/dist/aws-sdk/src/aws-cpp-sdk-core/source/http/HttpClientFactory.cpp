/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/internal/AWSHttpResourceClient.h>

#if AWS_SDK_USE_CRT_HTTP
#include <aws/core/http/crt/CRTHttpClient.h>
#include <aws/core/Globals.h>
#endif
#if ENABLE_CURL_CLIENT
#include <aws/core/http/curl/CurlHttpClient.h>
#include <signal.h>

#elif ENABLE_WINDOWS_CLIENT
#include <aws/core/client/ClientConfiguration.h>
#if ENABLE_WINDOWS_IXML_HTTP_REQUEST_2_CLIENT
#include <aws/core/http/windows/IXmlHttpRequest2HttpClient.h>
#if BYPASS_DEFAULT_PROXY
#include <aws/core/http/windows/WinHttpSyncHttpClient.h>
#endif
#else
#include <aws/core/http/windows/WinINetSyncHttpClient.h>
#include <aws/core/http/windows/WinHttpSyncHttpClient.h>
#endif
#endif

#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <cassert>

using namespace Aws::Client;
using namespace Aws::Http;
using namespace Aws::Utils::Logging;

namespace Aws
{
    namespace Http
    {
        static std::shared_ptr<HttpClientFactory>& GetHttpClientFactory()
        {
            static std::shared_ptr<HttpClientFactory> s_HttpClientFactory(nullptr);
            return s_HttpClientFactory;
        }
        static bool s_InitCleanupCurlFlag(false);
        static bool s_InstallSigPipeHandler(false);

        static const char* HTTP_CLIENT_FACTORY_ALLOCATION_TAG = "HttpClientFactory";

#if ENABLE_CURL_CLIENT && !defined(_WIN32)
        static void LogAndSwallowHandler(int)
        {
        }
#endif

        class DefaultHttpClientFactory : public HttpClientFactory
        {
            std::shared_ptr<HttpClient> CreateHttpClient(const ClientConfiguration& clientConfiguration) const override
            {
#if AWS_SDK_USE_CRT_HTTP
                return Aws::MakeShared<CRTHttpClient>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, clientConfiguration, *GetDefaultClientBootstrap());
                // Figure out whether the selected option is available but fail gracefully and return a default of some type if not
                // Windows clients:  Http and Inet are always options, Curl MIGHT be an option if USE_CURL_CLIENT is on, and http is "default"
                // Other clients: Curl is your default
#elif ENABLE_WINDOWS_CLIENT
#if ENABLE_WINDOWS_IXML_HTTP_REQUEST_2_CLIENT
#if BYPASS_DEFAULT_PROXY
                switch (clientConfiguration.httpLibOverride)
                {
                    case TransferLibType::WIN_HTTP_CLIENT:
                        AWS_LOGSTREAM_INFO(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "Creating WinHTTP http client.");
                        return Aws::MakeShared<WinHttpSyncHttpClient>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, clientConfiguration);
                    case TransferLibType::WIN_INET_CLIENT:
                        AWS_LOGSTREAM_WARN(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "WinINet http client is not supported with the current build configuration.");
                        // fall-through
                    default:
                        AWS_LOGSTREAM_INFO(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "Creating IXMLHttpRequest http client.");
                        return Aws::MakeShared<IXmlHttpRequest2HttpClient>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, clientConfiguration);
                }
#else
                return Aws::MakeShared<IXmlHttpRequest2HttpClient>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, clientConfiguration);
#endif // BYPASS_DEFAULT_PROXY
#else
                switch (clientConfiguration.httpLibOverride)
                {
                    case TransferLibType::WIN_INET_CLIENT:
                        return Aws::MakeShared<WinINetSyncHttpClient>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, clientConfiguration);

                    default:
                        return Aws::MakeShared<WinHttpSyncHttpClient>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, clientConfiguration);
                }
#endif // ENABLE_WINDOWS_IXML_HTTP_REQUEST_2_CLIENT
#elif ENABLE_CURL_CLIENT
                return Aws::MakeShared<CurlHttpClient>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, clientConfiguration);
#else
                // When neither of these clients is enabled, gcc gives a warning (converted
                // to error by -Werror) about the unused clientConfiguration parameter. We
                // prevent that warning with AWS_UNREFERENCED_PARAM.
                AWS_UNREFERENCED_PARAM(clientConfiguration);
                AWS_LOGSTREAM_WARN(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "SDK was built without an Http implementation, default http client factory can't create an Http client instance.");
                return nullptr;
#endif
            }

            std::shared_ptr<HttpRequest> CreateHttpRequest(const Aws::String &uri, HttpMethod method,
                                                           const Aws::IOStreamFactory &streamFactory) const override
            {
                return CreateHttpRequest(URI(uri), method, streamFactory);
            }

            std::shared_ptr<HttpRequest> CreateHttpRequest(const URI& uri, HttpMethod method, const Aws::IOStreamFactory& streamFactory) const override
            {
                auto request = Aws::MakeShared<Standard::StandardHttpRequest>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, uri, method);
                request->SetResponseStreamFactory(streamFactory);

                return request;
            }

            void InitStaticState() override
            {
                AWS_LOGSTREAM_DEBUG(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "Initializing Http Static State");
#if ENABLE_CURL_CLIENT
                if(s_InitCleanupCurlFlag)
                {
                    AWS_LOGSTREAM_DEBUG(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "Initializing Curl Http Client");
                    CurlHttpClient::InitGlobalState();
                }
#if !defined (_WIN32)
                if(s_InstallSigPipeHandler)
                {
                    ::signal(SIGPIPE, LogAndSwallowHandler);
                }
#endif
#elif ENABLE_WINDOWS_IXML_HTTP_REQUEST_2_CLIENT
                IXmlHttpRequest2HttpClient::InitCOM();
#endif
            }

            virtual void CleanupStaticState() override
            {
                AWS_LOGSTREAM_DEBUG(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "Cleanup Http Static State");
#if ENABLE_CURL_CLIENT
                if(s_InitCleanupCurlFlag)
                {
                    AWS_LOGSTREAM_DEBUG(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "Cleanup Curl Http Client");
                    CurlHttpClient::CleanupGlobalState();
                }
#endif
            }
        };

        void SetInitCleanupCurlFlag(bool initCleanupFlag)
        {
            s_InitCleanupCurlFlag = initCleanupFlag;
        }

        void SetInstallSigPipeHandlerFlag(bool install)
        {
            s_InstallSigPipeHandler = install;
        }

        void InitHttp()
        {
            if(!GetHttpClientFactory())
            {
                GetHttpClientFactory() = Aws::MakeShared<DefaultHttpClientFactory>(HTTP_CLIENT_FACTORY_ALLOCATION_TAG);
            }
            GetHttpClientFactory()->InitStaticState();
        }

        void CleanupHttp()
        {
            if(GetHttpClientFactory())
            {
                // EC2 metadata client uses http client from a factory, it will be invalidated
                Aws::Internal::CleanupEC2MetadataClient();

                GetHttpClientFactory()->CleanupStaticState();
                GetHttpClientFactory() = nullptr;
            }
        }

        void SetHttpClientFactory(const std::shared_ptr<HttpClientFactory>& factory)
        {
            bool recreateEC2Client = Aws::Internal::GetEC2MetadataClient() ? true : false;
            CleanupHttp();
            GetHttpClientFactory() = factory;

            if (recreateEC2Client)
            {
                Aws::Internal::InitEC2MetadataClient();
            }
        }

        std::shared_ptr<HttpClient> CreateHttpClient(const Aws::Client::ClientConfiguration& clientConfiguration)
        {
            assert(GetHttpClientFactory());
            auto client = GetHttpClientFactory()->CreateHttpClient(clientConfiguration);

            if (!client)
            {
                AWS_LOGSTREAM_FATAL(HTTP_CLIENT_FACTORY_ALLOCATION_TAG, "Initializing Http Client failed!");
                // assert just in case this is a misconfiguration at development time to make the dev's job easier.
                assert(false && "Http client initialization failed. Some client configuration parameters are probably invalid");
            }

            return client;
        }

        std::shared_ptr<HttpRequest> CreateHttpRequest(const Aws::String& uri, HttpMethod method, const Aws::IOStreamFactory& streamFactory)
        {
            assert(GetHttpClientFactory());
            return GetHttpClientFactory()->CreateHttpRequest(uri, method, streamFactory);
        }

        std::shared_ptr<HttpRequest> CreateHttpRequest(const URI& uri, HttpMethod method, const Aws::IOStreamFactory& streamFactory)
        {
            assert(GetHttpClientFactory());
            return GetHttpClientFactory()->CreateHttpRequest(uri, method, streamFactory);
        }
    }
}
