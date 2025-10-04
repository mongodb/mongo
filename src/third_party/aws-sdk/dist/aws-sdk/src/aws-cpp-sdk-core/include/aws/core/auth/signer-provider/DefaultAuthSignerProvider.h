/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/auth/signer-provider/AWSAuthSignerProviderBase.h>

#include <aws/core/auth/signer/AWSAuthV4Signer.h>


namespace Aws
{
    namespace Auth
    {
        class AWSCredentialsProvider;

        class AWS_CORE_API DefaultAuthSignerProvider : public AWSAuthSignerProvider
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
            DefaultAuthSignerProvider(const std::shared_ptr<AWSCredentialsProvider>& credentialsProvider,
                const Aws::String& serviceName, const Aws::String& region,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signingPolicy = Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::RequestDependent,
                bool urlEscapePath = true);
            explicit DefaultAuthSignerProvider(const std::shared_ptr<Aws::Client::AWSAuthSigner>& signer);
            void AddSigner(std::shared_ptr<Aws::Client::AWSAuthSigner>& signer) override;
            std::shared_ptr<Aws::Client::AWSAuthSigner> GetSigner(const Aws::String& signerName) const override;
            std::shared_ptr<AWSCredentialsProvider> GetCredentialsProvider() const override { return m_credentialsProvider; }
        protected:
            Aws::Vector<std::shared_ptr<Aws::Client::AWSAuthSigner>> m_signers;
            std::shared_ptr<AWSCredentialsProvider> m_credentialsProvider;
        };
    }
}
