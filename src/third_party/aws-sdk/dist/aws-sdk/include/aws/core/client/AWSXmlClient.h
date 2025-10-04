/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#if !defined(AWS_XML_CLIENT_H)
#define AWS_XML_CLIENT_H

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/AWSClient.h>

namespace Aws
{
    namespace Utils
    {
        namespace Xml
        {
            class XmlDocument;
        } // namespace Xml
    } // namespace Utils

    namespace Client
    {
        typedef Utils::Outcome<AmazonWebServiceResult<Utils::Xml::XmlDocument>, AWSError<CoreErrors>> XmlOutcome;

        /**
        *  AWSClient that handles marshalling xml response bodies. You would inherit from this class
        *  to create a client that uses Xml as its payload format.
        */
        class AWS_CORE_API AWSXMLClient : public AWSClient
        {
        public:
            typedef AWSClient BASECLASS;

            AWSXMLClient(const Aws::Client::ClientConfiguration& configuration,
                const std::shared_ptr<Aws::Client::AWSAuthSigner>& signer,
                const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller);

            AWSXMLClient(const Aws::Client::ClientConfiguration& configuration,
                const std::shared_ptr<Aws::Auth::AWSAuthSignerProvider>& signerProvider,
                const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller);

            virtual ~AWSXMLClient() = default;

        protected:
            /**
             * Converts/Parses an http response into a meaningful AWSError object. Using the XML message structure.
             */
            virtual AWSError<CoreErrors> BuildAWSError(const std::shared_ptr<Aws::Http::HttpResponse>& response) const override;

            /**
             * Returns an xml document or an error from the request. Does some marshalling xml and raw streams,
             * then just calls AttemptExhaustively.
             *
             * method defaults to POST
             */
            XmlOutcome MakeRequest(const Aws::AmazonWebServiceRequest& request,
                                   const Aws::Endpoint::AWSEndpoint& endpoint,
                                   Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                   const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                   const char* signerRegionOverride = nullptr,
                                   const char* signerServiceNameOverride = nullptr) const;

            XmlOutcome MakeRequest(const Aws::Endpoint::AWSEndpoint& endpoint,
                                   const char* requestName = "",
                                   Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                   const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                   const char* signerRegionOverride = nullptr,
                                   const char* signerServiceNameOverride = nullptr) const;

            /**
             * Returns an xml document or an error from the request. Does some marshalling xml and raw streams,
             * then just calls AttemptExhaustively.
             *
             * method defaults to POST
             */
            XmlOutcome MakeRequest(const Aws::Http::URI& uri,
                const Aws::AmazonWebServiceRequest& request,
                Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                const char* signerName = Aws::Auth::SIGV4_SIGNER,
                const char* signerRegionOverride = nullptr,
                const char* signerServiceNameOverride = nullptr) const;


            /**
             * Returns an xml document or an error from the request. Does some marshalling xml and raw streams,
             * then just calls AttemptExhaustively.
             *
             * requestName is used for metrics and defaults to empty string, to avoid empty names in metrics provide a valid
             * name.
             *
             * method defaults to POST
             */
            XmlOutcome MakeRequest(const Aws::Http::URI& uri,
                Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                const char* signerName = Aws::Auth::SIGV4_SIGNER,
                const char* requestName = "",
                const char* signerRegionOverride = nullptr,
                const char* signerServiceNameOverride = nullptr) const;

            /**
            * This is used for event stream response.
            */
            XmlOutcome MakeRequestWithEventStream(const Aws::Http::URI& uri,
                const Aws::AmazonWebServiceRequest& request,
                Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                const char* singerName = Aws::Auth::SIGV4_SIGNER,
                const char* signerRegionOverride = nullptr,
                const char* signerServiceNameOverride = nullptr) const;

            XmlOutcome MakeRequestWithEventStream(const Aws::AmazonWebServiceRequest& request,
                                                  const Aws::Endpoint::AWSEndpoint& endpoint,
                                                  Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                                                  const char* signerName = Aws::Auth::SIGV4_SIGNER,
                                                  const char* signerRegionOverride = nullptr,
                                                  const char* signerServiceNameOverride = nullptr) const;

            /**
            * This is used for event stream response.
            * requestName is used for metrics and defaults to empty string, to avoid empty names in metrics provide a valid
            * name.
            */
            XmlOutcome MakeRequestWithEventStream(const Aws::Http::URI& uri,
                Http::HttpMethod method = Http::HttpMethod::HTTP_POST,
                const char* signerName = Aws::Auth::SIGV4_SIGNER,
                const char* requestName = "",
                const char* signerRegionOverride = nullptr,
                const char* signerServiceNameOverride = nullptr) const;
        };

    } // namespace Client
} // namespace Aws

#endif // !defined(AWS_XML_CLIENT_H)