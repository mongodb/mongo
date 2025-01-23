/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <smithy/identity/identity/AwsCredentialIdentityBase.h>

namespace Aws {
    namespace S3 {
        class S3ExpressIdentity final: public smithy::AwsCredentialIdentityBase {
        public:
            S3ExpressIdentity() = default;

            inline S3ExpressIdentity(String accessKeyId,
                String secretKeyId,
                String sessionToken,
                const Utils::DateTime &expiration) :
                m_accessKeyId(std::move(accessKeyId)),
                m_secretKeyId(std::move(secretKeyId)),
                m_sessionToken(std::move(sessionToken)),
                m_expiration(expiration) {}

            const String &getAccessKeyId() const {
                return m_accessKeyId;
            }

            const String &getSecretKeyId() const {
                return m_secretKeyId;
            }

            const String &getSessionToken() const {
                return m_sessionToken;
            }

            const Utils::DateTime &getExpiration() const {
                return m_expiration;
            }

            String accessKeyId() const override
            {
                return getAccessKeyId();
            }

            String secretAccessKey() const override
            {
                return getSecretKeyId();
            }

            Crt::Optional<String> sessionToken() const override
            {
                return getSessionToken();
            }

            Crt::Optional<DateTime> expiration() const override
            {
                return getExpiration();
            }


        private:
            Aws::String m_accessKeyId;
            Aws::String m_secretKeyId;
            Aws::String m_sessionToken;
            Aws::Utils::DateTime m_expiration;
        };
    }
}
