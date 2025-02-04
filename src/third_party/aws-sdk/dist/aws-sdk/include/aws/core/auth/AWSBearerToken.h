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
        class AWS_CORE_API AWSBearerToken
        {
        public:
            /**
             * Initializes an empty token.
             * Empty token is not expired by default.
             * Token expires only if an expiration date is explicitly set.
             */
            AWSBearerToken() : m_expiration((std::chrono::time_point<std::chrono::system_clock>::max)())
            {
            }

            /**
             * Initializes object with token.
             * Expiration date is set to "never expire".
             */
            AWSBearerToken(const Aws::String& token) :
                m_token(token),
                m_expiration((std::chrono::time_point<std::chrono::system_clock>::max)())
            {
            }

            /**
             * Initializes object with token and expiration date.
             */
            AWSBearerToken(const Aws::String& token, Aws::Utils::DateTime expiration) :
                m_token(token),
                m_expiration(expiration)
            {
            }

            /**
             * If token has not been initialized or been initialized to empty value.
             * Expiration date does not affect the result of this function.
             */
            inline bool IsEmpty() const { return m_token.empty(); }

            inline bool IsExpired() const { return m_expiration <= Aws::Utils::DateTime::Now(); }

            inline bool IsExpiredOrEmpty() const { return IsEmpty() || IsExpired(); }

            /**
             * Gets the underlying token
             */
            inline const Aws::String& GetToken() const
            {
                return m_token;
            }

            /**
             * Gets the expiration date of the token
             */
            inline Aws::Utils::DateTime GetExpiration() const
            {
                return m_expiration;
            }

            /**
             * Sets the underlying token. Copies from the parameter token
             */
            inline void SetToken(const Aws::String& token)
            {
                m_token = token;
            }

            /**
             * Sets the underlying token. Moves from the parameter token
             */
            inline void SetToken(Aws::String&& token)
            {
                m_token = std::move(token);
            }

            /**
             * Sets the expiration date of the credential
             */
            inline void SetExpiration(const Aws::Utils::DateTime& expiration)
            {
                m_expiration = expiration;
            }

            /**
             * Sets the expiration date of the credential
             */
            inline void SetExpiration(Aws::Utils::DateTime&& expiration)
            {
                m_expiration = std::move(expiration);
            }

        private:
            Aws::String m_token;
            Aws::Utils::DateTime m_expiration;
        };
    }
}
