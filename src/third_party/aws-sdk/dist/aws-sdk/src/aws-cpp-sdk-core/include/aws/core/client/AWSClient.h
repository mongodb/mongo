/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#if !defined(AWS_CLIENT_H)
#define AWS_CLIENT_H

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/client/AWSUrlPresigner.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/crypto/Hash.h>
#include <aws/core/auth/AWSAuthSignerProvider.h>
#include <aws/core/endpoint/AWSEndpoint.h>
#include <smithy/interceptor/Interceptor.h>
#include <memory>
#include <atomic>

namespace Aws
{
    namespace Utils
    {
        template<typename R, typename E>
        class Outcome;

        namespace RateLimits
        {
            class RateLimiterInterface;
        } // namespace RateLimits

        namespace Crypto
        {
            class MD5;
        } // namespace Crypto
    } // namespace Utils

    namespace Http
    {
        class HttpClient;

        class HttpClientFactory;

        class HttpRequest;

        class HttpResponse;

        class URI;
    } // namespace Http

    namespace Auth
    {
        AWS_CORE_API extern const char SIGV4_SIGNER[];
        AWS_CORE_API extern const char NULL_SIGNER[];
    }

    class AmazonWebServiceRequest;

    namespace Client
    {
        template<typename ERROR_TYPE>
        class AWSError;
        class AWSErrorMarshaller;
        class AWSAuthSigner;
        struct ClientConfiguration;
        class RetryStrategy;

        typedef Utils::Outcome<std::shared_ptr<Aws::Http::HttpResponse>, AWSError<CoreErrors>> HttpResponseOutcome;
        typedef Utils::Outcome<AmazonWebServiceResult<Utils::Stream::ResponseStream>, AWSError<CoreErrors>> StreamOutcome;

        /**
         * Abstract AWS Client. Contains most of the functionality necessary to build an http request, get it signed, and send it across the wire.
         */
        class AWS_CORE_API AWSClient
        { 
        public:
            /**
             * configuration will be used for http client settings, retry strategy, throttles, and signing information.
             * supplied signer will be used for all requests, aws sdk clients will use default AuthV4Signer.
             * errorMarshaller tells the client how to convert error payloads into AWSError objects.
             */
            AWSClient(const Aws::Client::ClientConfiguration& configuration,
                      const std::shared_ptr<Aws::Client::AWSAuthSigner>& signer,
                      const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller);

            /**
             * Configuration will be used for http client settings, retry strategy, throttles, and signing information.
             * Pass a signer provider to determine the proper signer for a given request; AWS services will use
             * SigV4 signer. errorMarshaller tells the client how to convert error payloads into AWSError objects.
             */
            AWSClient(const Aws::Client::ClientConfiguration& configuration,
                      const std::shared_ptr<Aws::Auth::AWSAuthSignerProvider>& signerProvider,
                      const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller);

            virtual ~AWSClient() { };

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri and http method. expirationInSeconds defaults
             * to 0 which is the default 7 days. The implication of this function is using auth signer v4 to sign it.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {});

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri, http method and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days. The implication of this function is using auth signer v4 to sign it.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {});

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri and http method and region. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri, http method and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {});

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri and http method, region, and service name. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri, http method and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {});

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri and http method, region, service name and signer name. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, const char* signerName, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer. for the supplied uri, http method, region, service name, signer name and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, const char* signerName, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds = 0, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {});

            Aws::String GeneratePresignedUrl(const Aws::Endpoint::AWSEndpoint& endpoint,
                                             Aws::Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                             const Aws::Http::HeaderValueCollection& customizedHeaders = {},
                                             uint64_t expirationInSeconds = 0,
                                             const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                             const char* signerRegionOverride = nullptr,
                                             const char* signerServiceNameOverride = nullptr,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {});

            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request, const Aws::Http::URI& uri, Aws::Http::HttpMethod method,
                                             const Aws::Http::QueryStringParameterCollection& extraParams = Aws::Http::QueryStringParameterCollection(), long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request, const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName,
                                             const char* signerName, const Aws::Http::QueryStringParameterCollection& extraParams = Aws::Http::QueryStringParameterCollection(), long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request, const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName,
                                             const Aws::Http::QueryStringParameterCollection& extraParams = Aws::Http::QueryStringParameterCollection(), long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request, const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region,
                                             const Aws::Http::QueryStringParameterCollection& extraParams = Aws::Http::QueryStringParameterCollection(), long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            const std::shared_ptr<Aws::Http::HttpClient>& GetHttpClient() const { return m_httpClient; }

            /**
             * Stop all requests immediately.
             * In flight requests will likely fail.
             */
            void DisableRequestProcessing();

            /**
             * Enable/ReEnable requests.
             */
            void EnableRequestProcessing();

            inline virtual const char* GetServiceClientName() const { return m_serviceName.c_str(); }
            /**
             * service client name is part of userAgent.
             * We need to update userAgent after setting this name if it's not pre-set by customer.
             * Note: this API should only be called in your extended class constructors.
             */
            virtual void SetServiceClientName(const Aws::String& name);

            void AppendToUserAgent(const Aws::String& valueToAppend);
        protected:
            /**
             * Calls AttemptOneRequest until it either, succeeds, runs out of retries from the retry strategy,
             * or encounters and error that is not retryable.
             */
            HttpResponseOutcome AttemptExhaustively(const Aws::Http::URI& uri,
                                                    const Aws::AmazonWebServiceRequest& request,
                                                    Http::HttpMethod httpMethod,
                                                    const char* signerName,
                                                    const char* signerRegionOverride = nullptr,
                                                    const char* signerServiceNameOverride = nullptr) const;

            /**
             * Calls AttemptOneRequest until it either, succeeds, runs out of retries from the retry strategy,
             * or encounters and error that is not retryable. This method is for payloadless requests e.g. GET, DELETE, HEAD
             *
             * requestName is used for metrics and defaults to empty string, to avoid empty names in metrics provide a valid
             * name.
             */
            HttpResponseOutcome AttemptExhaustively(const Aws::Http::URI& uri,
                                                    Http::HttpMethod httpMethod,
                                                    const char* signerName,
                                                    const char* requestName = "",
                                                    const char* signerRegionOverride = nullptr,
                                                    const char* signerServiceNameOverride = nullptr) const;

            /**
             * Build an Http Request from the AmazonWebServiceRequest object. Signs the request, sends it across the wire
             * then reports the http response.
             */
            HttpResponseOutcome AttemptOneRequest(const std::shared_ptr<Http::HttpRequest>& httpRequest,
                                                  const Aws::AmazonWebServiceRequest& request,
                                                  const char* signerName,
                                                  const char* signerRegionOverride = nullptr,
                                                  const char* signerServiceNameOverride = nullptr) const;

            /**
             * Signs an Http Request, sends it across the wire
             * then reports the http response. This method is for payloadless requests e.g. GET, DELETE, HEAD
             *
             * requestName is used for metrics and defaults to empty string, to avoid empty names in metrics provide a valid
             * name.
             */
            HttpResponseOutcome AttemptOneRequest(const std::shared_ptr<Http::HttpRequest>& httpRequest,
                                                  const char* signerName,
                                                  const char* requestName = "",
                                                  const char* signerRegionOverride = nullptr,
                                                  const char* signerServiceNameOverride = nullptr) const;

            /**
             * This is used for structureless response payloads (file streams, binary data etc...). It calls AttemptExhaustively, but upon
             * return transfers ownership of the underlying stream for the http response to the caller.
             */
            StreamOutcome MakeRequestWithUnparsedResponse(const Aws::Http::URI& uri,
                                                          const Aws::AmazonWebServiceRequest& request,
                                                          Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                                          const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                                          const char* signerRegionOverride = nullptr,
                                                          const char* signerServiceNameOverride = nullptr) const;

            /**
             * This is used for structureless response payloads (file streams, binary data etc...). It calls AttemptExhaustively, but upon
             * return transfers ownership of the underlying stream for the http response to the caller.
             *
             * requestName is used for metrics and defaults to empty string, to avoid empty names in metrics provide a valid
             * name.
             */
            StreamOutcome MakeRequestWithUnparsedResponse(const Aws::Http::URI& uri,
                                                          Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                                          const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                                          const char* requestName = "",
                                                          const char* signerRegionOverride = nullptr,
                                                          const char* signerServiceNameOverride = nullptr) const;

            StreamOutcome MakeRequestWithUnparsedResponse(const Aws::AmazonWebServiceRequest& request,
                                                          const Aws::Endpoint::AWSEndpoint& endpoint,
                                                          Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                                          const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                                          const char* signerRegionOverride = nullptr,
                                                          const char* signerServiceNameOverride = nullptr) const;

            /**
             * Abstract.  Subclassing clients should override this to tell the client how to marshall error payloads
             */
            virtual AWSError<CoreErrors> BuildAWSError(const std::shared_ptr<Aws::Http::HttpResponse>& response) const = 0;

            /**
             * Transforms the AmazonWebServicesResult object into an HttpRequest.
             */
            virtual void BuildHttpRequest(const Aws::AmazonWebServiceRequest& request,
                                          const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest) const;

            /**
             *  Gets the underlying ErrorMarshaller for subclasses to use.
             */
            const std::shared_ptr<AWSErrorMarshaller>& GetErrorMarshaller() const
            {
                return m_errorMarshaller;
            }

            /**
             * Gets the corresponding signer from the signers map by name.
             */
            Aws::Client::AWSAuthSigner* GetSignerByName(const char* name) const;

            friend Aws::Client::AWSAuthSigner* AWSUrlPresigner::GetSignerByName(const char* name) const;

            std::shared_ptr<Auth::AWSCredentialsProvider> GetCredentialsProvider() const {
                 return m_signerProvider->GetCredentialsProvider();
            }
        protected:

            /**
              * Creates an HttpRequest instance with the given URI and sets the proper headers from the
              * AmazonWebRequest, and finally signs that request with the given the signer.
              * The similar member function BuildHttpRequest() does not sign the request.
              * This member function is used internally only by clients that perform requests (input operations) using
              * event-streams.
              */
            std::shared_ptr<Aws::Http::HttpRequest> BuildAndSignHttpRequest(const Aws::Http::URI& uri,
                                                                            const Aws::AmazonWebServiceRequest& request,
                                                                            Http::HttpMethod method, const char* signerName) const;

            /**
             * Performs the HTTP request via the HTTP client while enforcing rate limiters
             */
            std::shared_ptr<Aws::Http::HttpResponse> MakeHttpRequest(std::shared_ptr<Aws::Http::HttpRequest>& request) const;
            Aws::String m_region;

            /**
             * Adds "X-Amzn-Trace-Id" header with the value of _X_AMZN_TRACE_ID if both
             * environment variables AWS_LAMBDA_FUNCTION_NAME and _X_AMZN_TRACE_ID are set.
             * Does not add/modify header "X-Amzn-Trace-Id" if it is already set.
             */
            static void AppendRecursionDetectionHeader(std::shared_ptr<Aws::Http::HttpRequest> ioRequest);

            static CoreErrors GuessBodylessErrorType(Aws::Http::HttpResponseCode responseCode);
            static bool DoesResponseGenerateError(const std::shared_ptr<Aws::Http::HttpResponse>& response);
            std::shared_ptr<smithy::components::tracing::TelemetryProvider> m_telemetryProvider;
            std::shared_ptr<Aws::Auth::AWSAuthSignerProvider> m_signerProvider;
        private:
            /**
             * Try to adjust signer's clock
             * return true if signer's clock is adjusted, false otherwise.
             */
            bool AdjustClockSkew(HttpResponseOutcome& outcome, const char* signerName) const;
            void AddHeadersToRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest, const Http::HeaderValueCollection& headerValues) const;
            void AddContentBodyToRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest, const std::shared_ptr<Aws::IOStream>& body,
                                         bool needsContentMd5 = false, bool isChunked = false) const;
            void AddCommonHeaders(Aws::Http::HttpRequest& httpRequest) const;
            void AppendHeaderValueToRequest(const std::shared_ptr<Http::HttpRequest> &request, String header, String value) const;

            std::shared_ptr<Aws::Http::HttpClient> m_httpClient;
            std::shared_ptr<AWSErrorMarshaller> m_errorMarshaller;
            std::shared_ptr<RetryStrategy> m_retryStrategy;
            std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface> m_writeRateLimiter;
            std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface> m_readRateLimiter;
            Aws::String m_userAgent;
            std::shared_ptr<Aws::Utils::Crypto::Hash> m_hash;
            long m_requestTimeoutMs;
            bool m_enableClockSkewAdjustment;
            Aws::String m_serviceName = "AWSBaseClient";
            Aws::Client::RequestCompressionConfig m_requestCompressionConfig;
            Aws::Vector<std::shared_ptr<smithy::interceptor::Interceptor>> m_interceptors;
        };

        AWS_CORE_API Aws::String GetAuthorizationHeader(const Aws::Http::HttpRequest& httpRequest);
    } // namespace Client
} // namespace Aws

#if !defined(AWS_JSON_CLIENT_H) && !defined(AWS_XML_CLIENT_H)
/* Legacy backward compatibility macros to not break the build for ones including just AWSClient.h */
#include <aws/core/client/AWSJsonClient.h>
#include <aws/core/client/AWSXmlClient.h>
#endif // !defined(AWS_JSON_CLIENT_H) && !defined(AWS_XML_CLIENT_H)
#endif // !defined(AWS_CLIENT_H)