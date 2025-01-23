/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/config/AWSProfileConfigLoaderBase.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <fstream>

namespace Aws
{
    namespace Config
    {
        using namespace Aws::Utils;
        using namespace Aws::Auth;

        static const char* const CONFIG_LOADER_BASE_TAG = "Aws::Config::AWSProfileConfigLoaderBase";

        bool AWSProfileConfigLoader::Load()
        {
            if(LoadInternal())
            {
                AWS_LOGSTREAM_INFO(CONFIG_LOADER_BASE_TAG, "Successfully reloaded configuration.");
                m_lastLoadTime = DateTime::Now();
                AWS_LOGSTREAM_TRACE(CONFIG_LOADER_BASE_TAG, "reloaded config at "
                        << m_lastLoadTime.ToGmtString(DateFormat::ISO_8601));
                return true;
            }

            AWS_LOGSTREAM_INFO(CONFIG_LOADER_BASE_TAG, "Failed to reload configuration.");
            return false;
        }

        bool AWSProfileConfigLoader::PersistProfiles(const Aws::Map<Aws::String, Profile>& profiles)
        {
            if(PersistInternal(profiles))
            {
                AWS_LOGSTREAM_INFO(CONFIG_LOADER_BASE_TAG, "Successfully persisted configuration.");
                m_profiles = profiles;
                m_lastLoadTime = DateTime::Now();
                AWS_LOGSTREAM_TRACE(CONFIG_LOADER_BASE_TAG, "persisted config at "
                        << m_lastLoadTime.ToGmtString(DateFormat::ISO_8601));
                return true;
            }

            AWS_LOGSTREAM_WARN(CONFIG_LOADER_BASE_TAG, "Failed to persist configuration.");
            return false;
        }
    } // Config namespace
} // Aws namespace
