/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/s3/S3Errors.h>
#include <aws/s3/S3ExpressIdentityProvider.h>
#include <aws/core/client/AWSClient.h>
#include <aws/core/utils/ConcurrentCache.h>
#include <aws/core/http/HttpRequest.h>

namespace Aws {
    namespace S3 {
        class AWS_S3_API S3ExpressSigner : public Aws::Client::AWSAuthV4Signer {
        public:
            S3ExpressSigner(std::shared_ptr<S3ExpressIdentityProvider> S3ExpressIdentityProvider,
                const std::shared_ptr<Auth::AWSCredentialsProvider> &credentialsProvider,
                const Aws::String &serviceName,
                const Aws::String &region,
                PayloadSigningPolicy signingPolicy = PayloadSigningPolicy::RequestDependent,
                bool urlEscapePath = true,
                Aws::Auth::AWSSigningAlgorithm signingAlgorithm = Aws::Auth::AWSSigningAlgorithm::SIGV4);

            virtual ~S3ExpressSigner() {};

            const char *GetName() const override;

            bool SignRequest(Aws::Http::HttpRequest &request,
                const char *region,
                const char *serviceName,
                bool signBody
            ) const override;

            bool PresignRequest(Aws::Http::HttpRequest& request,
                const char* region,
                const char* serviceName,
                long long expirationInSeconds
            ) const override;

            Aws::Auth::AWSCredentials GetCredentials(const std::shared_ptr<Aws::Http::ServiceSpecificParameters> &serviceSpecificParameters) const override;

        protected:
            bool ServiceRequireUnsignedPayload(const String &serviceName) const override;

        private:
            inline bool hasRequestId(const Aws::String &requestId) const {
                std::lock_guard<std::mutex> lock(m_requestProcessing);
                return m_requestsProcessing.find(requestId) != m_requestsProcessing.end();
            }

            inline void putRequestId(const Aws::String &requestId) const {
                std::lock_guard<std::mutex> lock(m_requestProcessing);
                m_requestsProcessing.insert(requestId);
            }

            inline void deleteRequestId(const Aws::String &requestId) const {
                std::lock_guard<std::mutex> lock(m_requestProcessing);
                m_requestsProcessing.erase(requestId);
            }

            std::shared_ptr<S3ExpressIdentityProvider> m_S3ExpressIdentityProvider;
            std::shared_ptr<Auth::AWSCredentialsProvider> m_credentialsProvider;
            mutable std::set<Aws::String> m_requestsProcessing;
            mutable std::mutex m_requestProcessing;
            const Aws::String m_serviceName;
            const Aws::String m_region;
            const Aws::String m_endpoint;
        };
    }
}
