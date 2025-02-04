/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

 #pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
namespace Aws
{
    namespace Auth
    {
        /**
         * Simple data object around aws credentials
         */
        class AWS_CORE_API AWSCredentials
        {
        public:
            /**
             * Initializes an empty credentials set.
             * Empty credentials are not expired by default.
             * Credentials expire only if an expiration date is explicitly set on them.
             */
            AWSCredentials() : m_expiration((std::chrono::time_point<std::chrono::system_clock>::max)())
            {
            }

            /**
             * Initializes object with accessKeyId, secretKey.
             * SessionToken defaults to empty string.
             * Expiration date is set to "never expire".
             */
            AWSCredentials(const Aws::String& accessKeyId, const Aws::String& secretKey) :
                m_accessKeyId(accessKeyId), m_secretKey(secretKey), m_expiration((std::chrono::time_point<std::chrono::system_clock>::max)())
            {
            }

            /**
             * Initializes object with accessKeyId, secretKey, and sessionToken.
             * Expiration date is set to "never expire".
             */
            AWSCredentials(const Aws::String& accessKeyId, const Aws::String& secretKey, const Aws::String& sessionToken) :
                m_accessKeyId(accessKeyId), m_secretKey(secretKey), m_sessionToken(sessionToken), m_expiration((std::chrono::time_point<std::chrono::system_clock>::max)())
            {
            }

            /**
             * Initializes object with accessKeyId, secretKey, sessionToken and expiration date.
             */
            AWSCredentials(const Aws::String& accessKeyId, const Aws::String& secretKey, const Aws::String& sessionToken, Aws::Utils::DateTime expiration) :
                m_accessKeyId(accessKeyId), m_secretKey(secretKey), m_sessionToken(sessionToken), m_expiration(expiration)
            {
            }

            bool operator == (const AWSCredentials& other) const
            {
                return m_accessKeyId  == other.m_accessKeyId
                    && m_secretKey    == other.m_secretKey
                    && m_sessionToken == other.m_sessionToken
                    && m_expiration   == other.m_expiration;
            }

            bool operator != (const AWSCredentials& other) const
            {
                return !(other == *this);
            }

            /**
             * If credentials haven't been initialized or been initialized to empty values.
             * Expiration date does not affect the result of this function.
             */
            inline bool IsEmpty() const { return m_accessKeyId.empty() && m_secretKey.empty(); }

            inline bool IsExpired() const { return m_expiration <= Aws::Utils::DateTime::Now(); }

            inline bool IsExpiredOrEmpty() const { return IsEmpty() || IsExpired(); }

            /**
             * Gets the underlying access key credential
             */
            inline const Aws::String& GetAWSAccessKeyId() const
            {
                return m_accessKeyId;
            }

            /**
             * Gets the underlying secret key credential
             */
            inline const Aws::String& GetAWSSecretKey() const
            {
                return m_secretKey;
            }

            /**
             * Gets the underlying session token
             */
            inline const Aws::String& GetSessionToken() const
            {
                return m_sessionToken;
            }

            /**
             * Gets the expiration date of the credential
             */
            inline Aws::Utils::DateTime GetExpiration() const
            {
                return m_expiration;
            }

            /**
             * Sets the underlying access key credential. Copies from parameter accessKeyId.
             */
            inline void SetAWSAccessKeyId(const Aws::String& accessKeyId)
            {
                m_accessKeyId = accessKeyId;
            }

            /**
             * Sets the underlying secret key credential. Copies from parameter secretKey
             */
            inline void SetAWSSecretKey(const Aws::String& secretKey)
            {
                m_secretKey = secretKey;
            }

            /**
             * Sets the underlying session token. Copies from parameter sessionToken
             */
            inline void SetSessionToken(const Aws::String& sessionToken)
            {
                m_sessionToken = sessionToken;
            }


            /**
             * Sets the underlying access key credential. Copies from parameter accessKeyId.
             */
            inline void SetAWSAccessKeyId(const char* accessKeyId)
            {
                m_accessKeyId = accessKeyId;
            }

            /**
             * Sets the underlying secret key credential. Copies from parameter secretKey
             */
            inline void SetAWSSecretKey(const char* secretKey)
            {
                m_secretKey = secretKey;
            }

            /**
             * Sets the underlying secret key credential. Copies from parameter secretKey
             */
            inline void SetSessionToken(const char* sessionToken)
            {
                m_sessionToken = sessionToken;
            }

            /**
             * Sets the expiration date of the credential
             */
            inline void SetExpiration(Aws::Utils::DateTime expiration)
            {
                m_expiration = expiration;
            }

        private:
            Aws::String m_accessKeyId;
            Aws::String m_secretKey;
            Aws::String m_sessionToken;
            Aws::Utils::DateTime m_expiration;
        };
    }
}
