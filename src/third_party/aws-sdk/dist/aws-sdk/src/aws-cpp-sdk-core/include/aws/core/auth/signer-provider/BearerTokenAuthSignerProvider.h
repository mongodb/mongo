/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/auth/signer-provider/AWSAuthSignerProviderBase.h>
#include <aws/core/utils/memory/stl/AWSSet.h>
#include <aws/core/auth/signer/AWSAuthBearerSigner.h>


namespace Aws
{
    namespace Auth
    {
        class AWSCredentialsProvider;

        class AWS_CORE_API BearerTokenAuthSignerProvider : public AWSAuthSignerProvider
        {
        public:
            /**
             * Creates a Signature-V4 signer provider that supports the different implementations of Signature-V4
             * used for standard and event-stream requests.
             *
             * @param credentialsProvider A provider to retrieve the access/secret key used to derive the signing
             * @param serviceName The canonical name of the AWS service to be used in the signature
             * @param region The AWS region in which the requests will be made.
             */
            BearerTokenAuthSignerProvider() = delete;
            BearerTokenAuthSignerProvider(const std::shared_ptr<Aws::Auth::AWSBearerTokenProviderBase> bearerTokenProvider);
            void AddSigner(std::shared_ptr<Aws::Client::AWSAuthSigner>& signer) override;
            std::shared_ptr<Aws::Client::AWSAuthSigner> GetSigner(const Aws::String& signerName) const override;
        private:
            Aws::Vector<std::shared_ptr<Aws::Client::AWSAuthSigner>> m_signers;
        };
    }
}
