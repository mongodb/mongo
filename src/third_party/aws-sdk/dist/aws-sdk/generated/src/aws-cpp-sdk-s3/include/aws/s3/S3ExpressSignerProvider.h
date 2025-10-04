/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/auth/signer-provider/DefaultAuthSignerProvider.h>
#include <aws/s3/S3ExpressIdentityProvider.h>


namespace Aws {
    namespace Auth {
        class S3ExpressSignerProvider: public DefaultAuthSignerProvider {
        public:
            S3ExpressSignerProvider(const std::shared_ptr<AWSCredentialsProvider>& credentialsProvider,
                const std::shared_ptr<S3::S3ExpressIdentityProvider>& S3ExpressIdentityProvider,
                const Aws::String& serviceName,
                const Aws::String& region,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signingPolicy = Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::RequestDependent,
                bool urlEscapePath = true);
        };
    }
}
