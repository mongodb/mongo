/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#if !defined(AWS_JSON_CLIENT_H)
#define AWS_JSON_CLIENT_H

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/AWSClient.h>

namespace Aws
{
    namespace Utils
    {
        namespace Json
        {
            class JsonValue;
        } // namespace Json
    } // namespace Utils

    namespace Client
    {
        typedef Utils::Outcome<AmazonWebServiceResult<Utils::Json::JsonValue>, AWSError<CoreErrors>> JsonOutcome;
        /**
         *  AWSClient that handles marshalling json response bodies. You would inherit from this class
         *  to create a client that uses Json as its payload format.
         */
        class AWS_CORE_API AWSJsonClient : public AWSClient
        {
        public:
            typedef AWSClient BASECLASS;

            /**
             * Simply calls AWSClient constructor.
             */
            AWSJsonClient(const Aws::Client::ClientConfiguration& configuration,
                const std::shared_ptr<Aws::Client::AWSAuthSigner>& signer,
                const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller);

            /**
             * Simply calls AWSClient constructor.
             */
            AWSJsonClient(const Aws::Client::ClientConfiguration& configuration,
                    const std::shared_ptr<Aws::Auth::AWSAuthSignerProvider>& signerProvider,
                    const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller);

            virtual ~AWSJsonClient() = default;

        protected:
            /**
             * Converts/Parses an http response into a meaningful AWSError object using the json message structure.
             */
            virtual AWSError<CoreErrors> BuildAWSError(const std::shared_ptr<Aws::Http::HttpResponse>& response) const override;

            /**
             * Returns a Json document or an error from the request. Does some marshalling json and raw streams,
             * then just calls AttemptExhaustively.
             *
             * method defaults to POST
             */
            JsonOutcome MakeRequest(const Aws::AmazonWebServiceRequest& request,
                                    const Aws::Endpoint::AWSEndpoint& endpoint,
                                    Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                    const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                    const char* signerRegionOverride = nullptr,
                                    const char* signerServiceNameOverride = nullptr) const;

            JsonOutcome MakeRequest(const Aws::Endpoint::AWSEndpoint& endpoint,
                                    Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                    const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                    const char* signerRegionOverride = nullptr,
                                    const char* signerServiceNameOverride = nullptr) const;

            /**
             * Returns a Json document or an error from the request. Does some marshalling json and raw streams,
             * then just calls AttemptExhaustively.
             *
             * method defaults to POST
             */
            JsonOutcome MakeRequest(const Aws::Http::URI& uri,
                const Aws::AmazonWebServiceRequest& request,
                Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                const char* signerName = Aws::Auth::SIGV4_SIGNER,
                const char* signerRegionOverride = nullptr,
                const char* signerServiceNameOverride = nullptr) const;

            /**
             * Returns a Json document or an error from the request. Does some marshalling json and raw streams,
             * then just calls AttemptExhaustively.
             *
             * requestName is used for metrics and defaults to empty string, to avoid empty names in metrics provide a valid
             * name.
             *
             * method defaults to POST
             */
            JsonOutcome MakeRequest(const Aws::Http::URI& uri,
                Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                const char* signerName = Aws::Auth::SIGV4_SIGNER,
                const char* requestName = "",
                const char* signerRegionOverride = nullptr,
                const char* signerServiceNameOverride = nullptr) const;

            JsonOutcome MakeEventStreamRequest(std::shared_ptr<Aws::Http::HttpRequest>& request) const;
        };
    } // namespace Client
} // namespace Aws

#endif // !defined(AWS_JSON_CLIENT_H)