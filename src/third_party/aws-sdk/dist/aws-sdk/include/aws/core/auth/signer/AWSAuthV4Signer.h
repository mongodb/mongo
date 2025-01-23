/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include "aws/core/auth/signer/AWSAuthSignerBase.h"

#include <aws/core/utils/Array.h>
#include <aws/core/utils/memory/stl/AWSSet.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>
#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/crypto/Sha256HMAC.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/endpoint/internal/AWSEndpointAttribute.h>

#include <aws/crt/auth/Sigv4Signing.h>

#include <memory>

namespace smithy
{
    class AwsSigV4Signer;
}

namespace Aws
{
    namespace Http
    {
        class HttpRequest;
    } // namespace Http

    namespace Auth
    {
        class AWSCredentials;
        class AWSCredentialsProvider;

        enum class AWSSigningAlgorithm
        {
            SIGV4 = static_cast<int>(Aws::Crt::Auth::SigningAlgorithm::SigV4),
            ASYMMETRIC_SIGV4 = static_cast<int>(Aws::Crt::Auth::SigningAlgorithm::SigV4A),
        };

        AWS_CORE_API extern const char SIGV4_SIGNER[];
        AWS_CORE_API extern const char ASYMMETRIC_SIGV4_SIGNER[];
    } // namespace Auth

    namespace Client
    {
        /**
         * AWS Auth v4 Signer implementation of the AWSAuthSigner interface. More information on AWS Auth v4 Can be found here:
         * http://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html
         */
        class AWS_CORE_API AWSAuthV4Signer : public AWSAuthSigner
        {

        public:
            /**
             * Even though different payload signing polices, HTTP will force payload signing to be on.
             */
            enum class PayloadSigningPolicy
            {
                /**
                 * Sign the request based on the value returned by AmazonWebServiceRequest::SignBody()
                 */
                RequestDependent,
                /**
                 * Always sign the body of the request.
                 */
                Always,
                /**
                 * Never sign the body of the request
                 */
                Never
            };
            /**
             * credentialsProvider, source of AWS Credentials to sign requests with
             * serviceName,  canonical service name to sign with
             * region, region string to use in signature
             * signPayloads, if Always, the payload will have a sha256 computed on the body of the request. If this is set
             *    to Never, the sha256 will not be computed on the body. This is only useful for Amazon S3 over Https. If
             *    Https is not used then this flag will be ignored. If set to RequestDependent, compute or not is based on
             *    the value from AmazonWebServiceRequest::SignBody()
             */
            AWSAuthV4Signer(const std::shared_ptr<Auth::AWSCredentialsProvider>& credentialsProvider,
                            const char* serviceName, const Aws::String& region, PayloadSigningPolicy signingPolicy = PayloadSigningPolicy::RequestDependent,
                            bool urlEscapePath = true, Aws::Auth::AWSSigningAlgorithm signingAlgorithm = Aws::Auth::AWSSigningAlgorithm::SIGV4);

            virtual ~AWSAuthV4Signer();

            /**
             * AWSAuthV4signer's implementation of virtual function from base class
             * Return Auth Signer's name, here the value is specified in Aws::Auth::DEFAULT_AUTHV4_SIGNER.
             */
            const char* GetName() const override
            {
                if (m_signingAlgorithm == Aws::Auth::AWSSigningAlgorithm::ASYMMETRIC_SIGV4)
                {
                    return Aws::Auth::ASYMMETRIC_SIGV4_SIGNER;
                }
                else
                {
                    return Aws::Auth::SIGV4_SIGNER;
                }
            }

            /**
             * Signs the request itself based on info in the request and uri.
             * Uses AWS Auth V4 signing method with SHA256 HMAC algorithm.
             */
            bool SignRequest(Aws::Http::HttpRequest& request) const override
            {
                return SignRequest(request, m_region.c_str(), m_serviceName.c_str(), true/*signBody*/);
            }

            /**
            * Signs the request itself based on info in the request and uri.
            * Uses AWS Auth V4 signing method with SHA256 HMAC algorithm. If signBody is false
            * and https is being used then the body of the payload will not be signed.
            */
            bool SignRequest(Aws::Http::HttpRequest& request, bool signBody) const override
            {
                return SignRequest(request, m_region.c_str(), m_serviceName.c_str(), signBody);
            }

            /**
             * Uses AWS Auth V4 signing method with SHA256 HMAC algorithm. If signBody is false
             * and https is being used then the body of the payload will not be signed.
             * Using m_region by default if parameter region is nullptr.
             */
            bool SignRequest(Aws::Http::HttpRequest& request, const char* region, bool signBody) const override
            {
                return SignRequest(request, region, m_serviceName.c_str(), signBody);
            }

            /**
             * Uses AWS Auth V4 signing method with SHA256 HMAC algorithm. If signBody is false
             * and https is being used then the body of the payload will not be signed.
             * Using m_region by default if parameter region is nullptr.
             */
            bool SignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, bool signBody) const override;

            /**
            * Takes a request and signs the URI based on the HttpMethod, URI and other info from the request.
            * the region the signer was initialized with will be used for the signature.
            * The URI can then be used in a normal HTTP call until expiration.
            * Uses AWS Auth V4 signing method with SHA256 HMAC algorithm.
            * expirationInSeconds defaults to 0 which provides a URI good for 7 days.
            */
            bool PresignRequest(Aws::Http::HttpRequest& request, long long expirationInSeconds = 0) const override;

            /**
            * Takes a request and signs the URI based on the HttpMethod, URI and other info from the request.
            * The URI can then be used in a normal HTTP call until expiration.
            * Uses AWS Auth V4 signing method with SHA256 HMAC algorithm.
            * expirationInSeconds defaults to 0 which provides a URI good for 7 days.
            * Using m_region by default if parameter region is nullptr.
            */
            bool PresignRequest(Aws::Http::HttpRequest& request, const char* region, long long expirationInSeconds = 0) const override;

            /**
            * Takes a request and signs the URI based on the HttpMethod, URI and other info from the request.
            * The URI can then be used in a normal HTTP call until expiration.
            * Uses AWS Auth V4 signing method with SHA256 HMAC algorithm.
            * expirationInSeconds defaults to 0 which provides a URI good for 7 days.
            * Using m_region by default if parameter region is nullptr.
            * Using m_serviceName by default if parameter serviceName is nullptr.
            */
            bool PresignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, long long expirationInSeconds = 0) const override;

            virtual Aws::Auth::AWSCredentials GetCredentials(const std::shared_ptr<Aws::Http::ServiceSpecificParameters> &serviceSpecificParameters) const;

            Aws::String GetServiceName() const { return m_serviceName; }
            Aws::String GetRegion() const { return m_region; }
            Aws::String GenerateSignature(const Aws::Auth::AWSCredentials& credentials,
                    const Aws::String& stringToSign, const Aws::String& simpleDate) const;
            bool ShouldSignHeader(const Aws::String& header) const;

        protected:
            virtual bool ServiceRequireUnsignedPayload(const Aws::String& serviceName) const;
            bool m_includeSha256HashHeader;

        private:
            Aws::String GenerateSignature(const Aws::Auth::AWSCredentials& credentials,
                    const Aws::String& stringToSign, const Aws::String& simpleDate, const Aws::String& region,
                    const Aws::String& serviceName) const;

            Aws::String GenerateSignature(const Aws::String& stringToSign, const Aws::Utils::ByteBuffer& key) const;
            Aws::String ComputePayloadHash(Aws::Http::HttpRequest&) const;
            Aws::String GenerateStringToSign(const Aws::String& dateValue, const Aws::String& simpleDate,
                    const Aws::String& canonicalRequestHash, const Aws::String& region,
                    const Aws::String& serviceName) const;
            Aws::Utils::ByteBuffer ComputeHash(const Aws::String& secretKey,
                    const Aws::String& simpleDate, const Aws::String& region, const Aws::String& serviceName) const;
            bool SignRequestWithSigV4a(Aws::Http::HttpRequest& request, const char* region, const char* serviceName,
                    bool signBody, long long expirationTimeInSeconds, Aws::Crt::Auth::SignatureType signatureType) const;

            friend class smithy::AwsSigV4Signer;
            /**
             * Temporary method added for migration to the smithy architecture. Please do not use.
             */
            bool SignRequestWithCreds(Aws::Http::HttpRequest& request, const Auth::AWSCredentials& credentials,
                                      const char* region, const char* serviceName, bool signBody) const;


            Aws::Auth::AWSSigningAlgorithm m_signingAlgorithm;
            std::shared_ptr<Auth::AWSCredentialsProvider> m_credentialsProvider;
            const Aws::String m_serviceName;
            const Aws::String m_region;

            Aws::Set<Aws::String> m_unsignedHeaders;

            //these next four fields are ONLY for caching purposes and do not change
            //the logical state of the signer. They are marked mutable so the
            //interface can remain const.
            mutable Aws::Utils::ByteBuffer m_partialSignature;
            mutable Aws::String m_currentDateStr;
            mutable Aws::String m_currentSecretKey;
            mutable Utils::Threading::ReaderWriterLock m_partialSignatureLock;
            PayloadSigningPolicy m_payloadSigningPolicy;
            bool m_urlEscapePath;
            mutable Aws::Crt::Auth::Sigv4HttpRequestSigner m_crtSigner{};
        };
    } // namespace Client
} // namespace Aws

