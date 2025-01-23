/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/config/AWSProfileConfig.h>

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>

namespace Aws
{
    namespace Config {
        /**
         * Loads Configuration such as .aws/config, .aws/credentials or ec2 metadata service.
         */
        class AWS_CORE_API AWSProfileConfigLoader
        {
        public:
            virtual ~AWSProfileConfigLoader() = default;

            /**
             * Load the configuration
             */
            bool Load();

            /**
             * Over writes the entire config source with the newly configured profile data.
             */
            bool PersistProfiles(const Aws::Map<Aws::String, Aws::Config::Profile> &profiles);

            /**
             * Gets all profiles from the configuration file.
             */
            inline const Aws::Map<Aws::String, Aws::Config::Profile> &GetProfiles() const { return m_profiles; };

            /**
             * the timestamp from the last time the profile information was loaded from file.
             */
            inline const Aws::Utils::DateTime &LastLoadTime() const { return m_lastLoadTime; }

            using ProfilesContainer = Aws::Map<Aws::String, Aws::Config::Profile>;

            // Delete copy c-tor and assignment operator
            AWSProfileConfigLoader() = default;

            AWSProfileConfigLoader(const AWSProfileConfigLoader &) = delete;

            const AWSProfileConfigLoader &operator=(AWSProfileConfigLoader &) = delete;

        protected:
            /**
             * Subclasses override this method to implement fetching the profiles.
             */
            virtual bool LoadInternal() = 0;

            /**
             * Subclasses override this method to implement persisting the profiles. Default returns false.
             */
            virtual bool PersistInternal(const Aws::Map<Aws::String, Aws::Config::Profile> &) { return false; }

            ProfilesContainer m_profiles;
            Aws::Utils::DateTime m_lastLoadTime;
        };
    }
}
