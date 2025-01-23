/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include "aws/core/auth/signer/AWSAuthSignerBase.h"

namespace Aws
{
    namespace Http
    {
        class HttpRequest;
    } // namespace Http

    namespace Utils
    {
        namespace Event
        {
            class Message;
        }
    } // namespace Utils

    namespace Auth
    {
        class AWSBearerTokenProviderBase;
        AWS_CORE_API extern const char BEARER_SIGNER[];
    } // namespace Auth

    namespace Client
    {
        class AWS_CORE_API AWSAuthBearerSigner : public AWSAuthSigner
        {

        public:
            /**
             * An implementation of a signer interface that uses bearer token auth signature.
             */
            AWSAuthBearerSigner(const std::shared_ptr<Aws::Auth::AWSBearerTokenProviderBase> bearerTokenProvider)
              : m_bearerTokenProvider(bearerTokenProvider)
            {}

            virtual ~AWSAuthBearerSigner() {};

            /**
             * Return the signer's name
             */
            const char* GetName() const override
            {
                return Aws::Auth::BEARER_SIGNER;
            }

            /**
             * Sign request with a bearer auth token
             * @return true if success, false if fail to sign
             */
            bool SignRequest(Aws::Http::HttpRequest& ) const override;

            /**
             * Dummy function to satisfy the interface requirements of a base Signer interface
             *   additional arguments are not used.
             * @return true if success, false if fail to sign
             */
            bool SignRequest(Aws::Http::HttpRequest& ioRequest, const char* /*region*/, const char* /*serviceName*/, bool /*signBody*/) const override
            {
                return SignRequest(ioRequest);
            }

            /**
             * Dummy function to satisfy the interface requirements of a base Signer interface
             * @return true if success, false if fail to sign
             */
            bool PresignRequest(Aws::Http::HttpRequest& ioRequest, long long /*expirationInSeconds = 0*/) const override
            {
                return SignRequest(ioRequest);
            }

            /**
             * Dummy function to satisfy the interface requirements of a base Signer interface
             *   additional arguments are not used.
             * @return true if success, false if fail to sign
             */
            bool PresignRequest(Aws::Http::HttpRequest& ioRequest, const char* /*region*/, long long expirationInSeconds = 0) const override
            {
                return PresignRequest(ioRequest, expirationInSeconds);
            }

            /**
             * Dummy function to satisfy the interface requirements of a base Signer interface
             *   additional arguments are not used.
             * @return true if success, false if fail to sign
             */
            bool PresignRequest(Aws::Http::HttpRequest& ioRequest, const char* /*region*/, const char* /*serviceName*/, long long expirationInSeconds = 0) const override
            {
                return PresignRequest(ioRequest, expirationInSeconds);
            }

        protected:
            std::shared_ptr<Aws::Auth::AWSBearerTokenProviderBase> m_bearerTokenProvider;
        };

    } // namespace Client
} // namespace Aws

