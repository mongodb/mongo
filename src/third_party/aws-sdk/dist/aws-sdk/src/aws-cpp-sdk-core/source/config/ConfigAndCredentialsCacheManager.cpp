/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/config/ConfigAndCredentialsCacheManager.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/utils/memory/stl/AWSList.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <fstream>

namespace Aws
{
    namespace Config
    {
        using namespace Aws::Utils;
        using namespace Aws::Auth;

        #ifdef _MSC_VER
            // VS2015 compiler's bug, warning s_CoreErrorsMapper: symbol will be dynamically initialized (implementation limitation)
            AWS_SUPPRESS_WARNING(4592,
                static ConfigAndCredentialsCacheManager* s_configManager(nullptr);
            )
        #else
            static ConfigAndCredentialsCacheManager* s_configManager(nullptr);
        #endif

        static const char CONFIG_CREDENTIALS_CACHE_MANAGER_TAG[] = "ConfigAndCredentialsCacheManager";


        ConfigAndCredentialsCacheManager::ConfigAndCredentialsCacheManager() :
                m_credentialsFileLoader(Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetCredentialsProfileFilename()),
                m_configFileLoader(Aws::Auth::GetConfigProfileFilename(), true/*use profile prefix*/)
        {
            ReloadCredentialsFile();
            ReloadConfigFile();
        }

        void ConfigAndCredentialsCacheManager::ReloadConfigFile()
        {
            Aws::Utils::Threading::WriterLockGuard guard(m_configLock);
            m_configFileLoader.SetFileName(Aws::Auth::GetConfigProfileFilename());
            m_configFileLoader.Load();
        }

        void ConfigAndCredentialsCacheManager::ReloadCredentialsFile()
        {
            Aws::Utils::Threading::WriterLockGuard guard(m_credentialsLock);
            m_credentialsFileLoader.SetFileName(Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetCredentialsProfileFilename());
            m_credentialsFileLoader.Load();
        }

        bool ConfigAndCredentialsCacheManager::HasConfigProfile(const Aws::String& profileName) const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_configLock);
            return (m_configFileLoader.GetProfiles().count(profileName) == 1);
        }

        Aws::Config::Profile ConfigAndCredentialsCacheManager::GetConfigProfile(const Aws::String& profileName) const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_configLock);
            const auto& profiles = m_configFileLoader.GetProfiles();
            const auto &iter = profiles.find(profileName);
            if (iter == profiles.end())
            {
                return {};
            }
            return iter->second;
        }

        Aws::Map<Aws::String, Aws::Config::Profile> ConfigAndCredentialsCacheManager::GetConfigProfiles() const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_configLock);
            return m_configFileLoader.GetProfiles();
        }

        Aws::String ConfigAndCredentialsCacheManager::GetConfig(const Aws::String& profileName, const Aws::String& key) const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_configLock);
            const auto& profiles = m_configFileLoader.GetProfiles();
            const auto &iter = profiles.find(profileName);
            if (iter == profiles.end())
            {
                return {};
            }
            return iter->second.GetValue(key);
        }

        bool ConfigAndCredentialsCacheManager::HasCredentialsProfile(const Aws::String& profileName) const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_credentialsLock);
            return (m_credentialsFileLoader.GetProfiles().count(profileName) == 1);
        }

        Aws::Config::Profile ConfigAndCredentialsCacheManager::GetCredentialsProfile(const Aws::String& profileName) const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_credentialsLock);
            const auto &profiles = m_credentialsFileLoader.GetProfiles();
            const auto &iter = profiles.find(profileName);
            if (iter == profiles.end())
            {
                return {};
            }
            return iter->second;
        }

        Aws::Map<Aws::String, Aws::Config::Profile> ConfigAndCredentialsCacheManager::GetCredentialsProfiles() const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_credentialsLock);
            return m_credentialsFileLoader.GetProfiles();
        }

        Aws::Auth::AWSCredentials ConfigAndCredentialsCacheManager::GetCredentials(const Aws::String& profileName) const
        {
            Aws::Utils::Threading::ReaderLockGuard guard(m_credentialsLock);
            const auto& profiles = m_credentialsFileLoader.GetProfiles();
            const auto &iter = profiles.find(profileName);
            if (iter == profiles.end())
            {
                return {};
            }
            return iter->second.GetCredentials();
        }

        void InitConfigAndCredentialsCacheManager()
        {
            if (s_configManager)
            {
                return;
            }
            s_configManager = Aws::New<ConfigAndCredentialsCacheManager>(CONFIG_CREDENTIALS_CACHE_MANAGER_TAG);
        }

        void CleanupConfigAndCredentialsCacheManager()
        {
            Aws::Delete(s_configManager);
            s_configManager = nullptr;
        }

        void ReloadCachedConfigFile()
        {
            assert(s_configManager);
            s_configManager->ReloadConfigFile();
        }

        void ReloadCachedCredentialsFile()
        {
            assert(s_configManager);
            s_configManager->ReloadCredentialsFile();
        }

        bool HasCachedConfigProfile(const Aws::String& profileName)
        {
            assert(s_configManager);
            return s_configManager->HasConfigProfile(profileName);
        }

        Aws::Config::Profile GetCachedConfigProfile(const Aws::String& profileName)
        {
            assert(s_configManager);
            return s_configManager->GetConfigProfile(profileName);
        }

        Aws::Map<Aws::String, Aws::Config::Profile> GetCachedConfigProfiles()
        {
            assert(s_configManager);
            return s_configManager->GetConfigProfiles();
        }

        Aws::String GetCachedConfigValue(const Aws::String &profileName, const Aws::String &key)
        {
            assert(s_configManager);
            return s_configManager->GetConfig(profileName, key);
        }

        Aws::String GetCachedConfigValue(const Aws::String &key)
        {
            assert(s_configManager);
            return s_configManager->GetConfig(Aws::Auth::GetConfigProfileName(), key);
        }

        bool HasCachedCredentialsProfile(const Aws::String& profileName)
        {
            assert(s_configManager);
            return s_configManager->HasCredentialsProfile(profileName);
        }

        Aws::Config::Profile GetCachedCredentialsProfile(const Aws::String &profileName)
        {
            assert(s_configManager);
            return s_configManager->GetCredentialsProfile(profileName);
        }

        Aws::Map<Aws::String, Aws::Config::Profile> GetCachedCredentialsProfiles()
        {
            assert(s_configManager);
            return s_configManager->GetCredentialsProfiles();
        }

        Aws::Auth::AWSCredentials GetCachedCredentials(const Aws::String &profileName)
        {
            assert(s_configManager);
            return s_configManager->GetCredentials(profileName);
        }
    } // Config namespace
} // Aws namespace
