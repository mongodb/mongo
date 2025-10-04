/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/config/AWSProfileConfigLoader.h>

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>

namespace Aws
{
    namespace Config
    {
        /**
         * Stores the contents of config file and credentials file to avoid multiple file readings.
         * At the same time provides the flexibility to reload from file.
         */
        class AWS_CORE_API ConfigAndCredentialsCacheManager
        {
        public:
            ConfigAndCredentialsCacheManager();

            void ReloadConfigFile();

            void ReloadCredentialsFile();

            bool HasConfigProfile(const Aws::String& profileName) const;

            /**
             * Returns cached config profile with the specified profile name.
             * Using copy instead of const reference to avoid reading bad contents due to thread contention.
             */
            Aws::Config::Profile GetConfigProfile(const Aws::String& profileName) const;

            /**
             * Returns cached config profiles
             * Using copy instead of const reference to avoid reading bad contents due to thread contention.
             */
            Aws::Map<Aws::String, Aws::Config::Profile> GetConfigProfiles() const;

            /**
             * Returns cached config value with the specified profile name and key.
             * Using copy instead of const reference to avoid reading bad contents due to thread contention.
             */
            Aws::String GetConfig(const Aws::String& profileName, const Aws::String& key) const;

            bool HasCredentialsProfile(const Aws::String& profileName) const;
            /**
             * Returns cached credentials profile with the specified profile name.
             * Using copy instead of const reference to avoid reading bad contents due to thread contention.
             */
            Aws::Config::Profile GetCredentialsProfile(const Aws::String& profileName) const;

            /**
             * Returns cached credentials profiles.
             * Using copy instead of const reference to avoid reading bad contents due to thread contention.
             */
            Aws::Map<Aws::String, Aws::Config::Profile> GetCredentialsProfiles() const;

            /**
             * Returns cached credentials with the specified profile name.
             * Using copy instead of const reference to avoid reading bad contents due to thread contention.
             */
            Aws::Auth::AWSCredentials GetCredentials(const Aws::String& profileName) const;

        private:
            mutable Aws::Utils::Threading::ReaderWriterLock m_credentialsLock;
            Aws::Config::AWSConfigFileProfileConfigLoader m_credentialsFileLoader;
            mutable Aws::Utils::Threading::ReaderWriterLock m_configLock;
            Aws::Config::AWSConfigFileProfileConfigLoader m_configFileLoader;
        };

        AWS_CORE_API void InitConfigAndCredentialsCacheManager();

        AWS_CORE_API void CleanupConfigAndCredentialsCacheManager();

        AWS_CORE_API void ReloadCachedConfigFile();

        AWS_CORE_API void ReloadCachedCredentialsFile();

        AWS_CORE_API bool HasCachedConfigProfile(const Aws::String& profileName);

        AWS_CORE_API Aws::Config::Profile GetCachedConfigProfile(const Aws::String& profileName);

        AWS_CORE_API Aws::Map<Aws::String, Aws::Config::Profile> GetCachedConfigProfiles();

        AWS_CORE_API Aws::String GetCachedConfigValue(const Aws::String& profileName, const Aws::String& key);

        AWS_CORE_API Aws::String GetCachedConfigValue(const Aws::String& key);

        AWS_CORE_API bool HasCachedCredentialsProfile(const Aws::String &profileName);

        AWS_CORE_API Aws::Config::Profile GetCachedCredentialsProfile(const Aws::String& profileName);

        AWS_CORE_API Aws::Auth::AWSCredentials GetCachedCredentials(const Aws::String& profileName);

        AWS_CORE_API Aws::Map<Aws::String, Aws::Config::Profile> GetCachedCredentialsProfiles();
    }
}
