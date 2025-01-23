/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/auth/AWSCredentials.h>

namespace Aws
{
    namespace Config
    {
        /**
         * Simple data container for a Profile.
         */
        class Profile
        {
        public:
            /*
             * Data container for a sso-session config entry.
             * This is independent of the general profile configuration and used by a bearer auth token provider.
             */
            class SsoSession
            {
            public:
                inline const Aws::String& GetName() const { return m_name; }
                inline void SetName(const Aws::String& value) { m_name = value; }
                inline const Aws::String& GetSsoRegion() const { return m_ssoRegion; }
                inline void SetSsoRegion(const Aws::String& value) { m_ssoRegion = value; }
                inline const Aws::String& GetSsoStartUrl() const { return m_ssoStartUrl; }
                inline void SetSsoStartUrl(const Aws::String& value) { m_ssoStartUrl = value; }

                inline void SetAllKeyValPairs(const Aws::Map<Aws::String, Aws::String>& map) { m_allKeyValPairs = map; }
                inline const Aws::String GetValue(const Aws::String& key) const
                {
                    auto iter = m_allKeyValPairs.find(key);
                    if (iter == m_allKeyValPairs.end()) return {};
                    return iter->second;
                }

                bool operator==(SsoSession const& other) const
                {
                    return this->m_name == other.m_name &&
                            this->m_ssoRegion == other.m_ssoRegion &&
                            this->m_ssoStartUrl == other.m_ssoStartUrl &&
                            this->m_allKeyValPairs == other.m_allKeyValPairs;
                }
                bool operator!=(SsoSession const& other) const
                {
                    return !operator==(other);
                }
            private:
                // This is independent of the general configuration
                Aws::String m_name;
                Aws::String m_ssoRegion;
                Aws::String m_ssoStartUrl;
                Aws::Map<Aws::String, Aws::String> m_allKeyValPairs;
            };

            inline const Aws::String& GetName() const { return m_name; }
            inline void SetName(const Aws::String& value) { m_name = value; }
            inline const Aws::Auth::AWSCredentials& GetCredentials() const { return m_credentials; }
            inline void SetCredentials(const Aws::Auth::AWSCredentials& value) { m_credentials = value; }
            inline const Aws::String& GetRegion() const { return m_region; }
            inline void SetRegion(const Aws::String& value) { m_region = value; }
            inline const Aws::String& GetRoleArn() const { return m_roleArn; }
            inline void SetRoleArn(const Aws::String& value) { m_roleArn = value; }
            inline const Aws::String& GetExternalId() const { return m_externalId; }
            inline void SetExternalId(const Aws::String& value) { m_externalId = value; }
            inline const Aws::String& GetSsoStartUrl() const { return m_ssoStartUrl; }
            inline void SetSsoStartUrl(const Aws::String& value) { m_ssoStartUrl = value; }
            inline const Aws::String& GetSsoRegion() const { return m_ssoRegion; }
            inline void SetSsoRegion(const Aws::String& value) { m_ssoRegion = value; }
            inline const Aws::String& GetSsoAccountId() const { return m_ssoAccountId; }
            inline void SetSsoAccountId(const Aws::String& value) { m_ssoAccountId = value; }
            inline const Aws::String& GetSsoRoleName() const { return m_ssoRoleName; }
            inline void SetSsoRoleName(const Aws::String& value) { m_ssoRoleName = value; }
            inline const Aws::String& GetDefaultsMode() const { return m_defaultsMode; }
            inline void SetDefaultsMode(const Aws::String& value) { m_defaultsMode = value; }
            inline const Aws::String& GetSourceProfile() const { return m_sourceProfile; }
            inline void SetSourceProfile(const Aws::String& value ) { m_sourceProfile = value; }
            inline const Aws::String& GetCredentialProcess() const { return m_credentialProcess; }
            inline void SetCredentialProcess(const Aws::String& value ) { m_credentialProcess = value; }
            inline void SetAllKeyValPairs(const Aws::Map<Aws::String, Aws::String>& map) { m_allKeyValPairs = map; }
            inline void SetAllKeyValPairs(Aws::Map<Aws::String, Aws::String>&& map) { m_allKeyValPairs = std::move(map); }
            inline const Aws::String GetValue(const Aws::String& key) const
            {
                auto iter = m_allKeyValPairs.find(key);
                if (iter == m_allKeyValPairs.end()) return {};
                return iter->second;
            }

            inline bool IsSsoSessionSet() const { return m_ssoSessionSet; }
            inline const SsoSession& GetSsoSession() const { return m_ssoSession; }
            inline void SetSsoSession(const SsoSession& value) { m_ssoSessionSet = true; m_ssoSession = value; }
            inline void SetSsoSession(SsoSession&& value) { m_ssoSessionSet = true; m_ssoSession = std::move(value); }

        private:
            Aws::String m_name;
            Aws::String m_region;
            Aws::Auth::AWSCredentials m_credentials;
            Aws::String m_roleArn;
            Aws::String m_externalId;
            Aws::String m_sourceProfile;
            Aws::String m_credentialProcess;
            Aws::String m_ssoStartUrl;
            Aws::String m_ssoRegion;
            Aws::String m_ssoAccountId;
            Aws::String m_ssoRoleName;
            Aws::String m_defaultsMode;
            Aws::Map<Aws::String, Aws::String> m_allKeyValPairs;

            bool m_ssoSessionSet = false;
            SsoSession m_ssoSession;
        };
    }
}
