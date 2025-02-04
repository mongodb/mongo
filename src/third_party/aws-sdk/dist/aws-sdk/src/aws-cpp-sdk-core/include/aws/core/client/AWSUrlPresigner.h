/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/endpoint/AWSEndpoint.h>

#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
    namespace Client
    {
        class AWSClient;

        /**
         * Helper class to generate pre-signed AWS URLs.
         */
        class AWS_CORE_API AWSUrlPresigner
        {
        public:
            AWSUrlPresigner(const AWSClient& client);

            virtual ~AWSUrlPresigner() {};

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri and http method. expirationInSeconds defaults
             * to 0 which is the default 7 days. The implication of this function is using auth signer v4 to sign it.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri, http method, and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days. The implication of this function is using auth signer v4 to sign it.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const Aws::Http::HeaderValueCollection &customizedHeaders,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri, http method, and region. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri, http method, and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const Aws::Http::HeaderValueCollection &customizedHeaders,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri, http method, region, and service name. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const char *serviceNameOverride,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri, http method, and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const char *serviceNameOverride,
                                             const Aws::Http::HeaderValueCollection &customizedHeaders,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri, http method, region, service name, and signer name. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const char *serviceNameOverride,
                                             const char *signerName,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer, for the supplied uri, http method, region, service name, signer name, and customized headers. expirationInSeconds defaults
             * to 0 which is the default 7 days.
             *
             * This is a real method for uri pre-signing, the rest are just overloads.
             */
            Aws::String GeneratePresignedUrl(const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const char *serviceNameOverride,
                                             const char *signerName,
                                             const Aws::Http::HeaderValueCollection &customizedHeaders,
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri for a supplied AWSEndpoint.
             */
            Aws::String GeneratePresignedUrl(const Aws::Endpoint::AWSEndpoint &endpoint,
                                             Aws::Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                             const Aws::Http::HeaderValueCollection &customizedHeaders = {},
                                             uint64_t expirationInSeconds = 0,
                                             const char *signerName = Aws::Auth::SIGV4_SIGNER,
                                             const char *signerRegionOverride = nullptr,
                                             const char *signerServiceNameOverride = nullptr,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri for a supplied request and uri.
             */
            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest &request,
                                             const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const Aws::Http::QueryStringParameterCollection &extraParams = Aws::Http::QueryStringParameterCollection(),
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri using the injected signer. for the supplied request object, uri, http method, region, service name, signer name, and customized headers.
             * expirationInSeconds defaults to 0 which is the default 7 days.
             *
             * This is a real method for request+uri pre-signing, the rest are just overloads.
             */
            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest &request,
                                             const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const char *serviceNameOverride,
                                             const char *signerName,
                                             const Aws::Http::QueryStringParameterCollection &extraParams = Aws::Http::QueryStringParameterCollection(),
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri for a supplied request and uri.
             */
            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest &request,
                                             const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const char *serviceNameOverride,
                                             const Aws::Http::QueryStringParameterCollection &extraParams = Aws::Http::QueryStringParameterCollection(),
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

            /**
             * Generates a signed Uri for a supplied request and uri.
             */
            Aws::String GeneratePresignedUrl(const Aws::AmazonWebServiceRequest &request,
                                             const Aws::Http::URI &uri,
                                             Aws::Http::HttpMethod method,
                                             const char *regionOverride,
                                             const Aws::Http::QueryStringParameterCollection &extraParams = Aws::Http::QueryStringParameterCollection(),
                                             long long expirationInSeconds = 0,
                                             const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter = {}) const;

        protected:
            const AWSClient& m_awsClient;

            friend class AWSClient; // allow AWSClient to see method below to make friends with it
            Aws::Client::AWSAuthSigner* GetSignerByName(const char* name) const;
        }; // class AWSUrlPresigner

    } // namespace Client
} // namespace Aws
