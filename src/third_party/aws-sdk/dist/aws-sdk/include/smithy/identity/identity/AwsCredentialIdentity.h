/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/identity/AwsCredentialIdentityBase.h>

namespace smithy {
    class AwsCredentialIdentity : public AwsCredentialIdentityBase {
    public:
        AwsCredentialIdentity(const Aws::String& accessKeyId,
                              const Aws::String& secretAccessKey,
                              const Aws::Crt::Optional<Aws::String>& sessionToken,
                              const Aws::Crt::Optional<AwsIdentity::DateTime>& expiration)
            : m_accessKeyId(accessKeyId), m_secretAccessKey(secretAccessKey),
              m_sessionToken(sessionToken), m_expiration(expiration) {}

        Aws::String accessKeyId() const override;
        Aws::String secretAccessKey() const override;
        Aws::Crt::Optional<Aws::String> sessionToken() const override;
        Aws::Crt::Optional<AwsIdentity::DateTime> expiration() const override;

    protected:
        Aws::String m_accessKeyId;
        Aws::String m_secretAccessKey;
        Aws::Crt::Optional<Aws::String> m_sessionToken;
        Aws::Crt::Optional<AwsIdentity::DateTime> m_expiration;
    };
}

#include <smithy/identity/identity/impl/AwsCredentialIdentityImpl.h>
