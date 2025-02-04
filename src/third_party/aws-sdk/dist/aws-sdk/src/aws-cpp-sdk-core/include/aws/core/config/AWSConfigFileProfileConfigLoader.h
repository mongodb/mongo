/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/config/AWSProfileConfigLoaderBase.h>

#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
    namespace Config
    {
        /**
         * Reads configuration from a config file (e.g. $HOME/.aws/config or $HOME/.aws/credentials
         */
        class AWS_CORE_API AWSConfigFileProfileConfigLoader : public AWSProfileConfigLoader
        {
        public:
            /**
             * fileName - file to load config from
             * useProfilePrefix - whether or not the profiles are prefixed with "profile", credentials file is not
             * while the config file is. Defaults to off.
             */
            AWSConfigFileProfileConfigLoader(const Aws::String& fileName, bool useProfilePrefix = false);

            virtual ~AWSConfigFileProfileConfigLoader() = default;

            /**
             * File path being used for the config loader.
             */
            const Aws::String& GetFileName() const { return m_fileName; }

            /**
             * Give loader the ability to change the file path to load config from.
             * This can avoid creating new loader object if the file changed.
             */
            void SetFileName(const Aws::String& fileName) { m_fileName = fileName; }

        protected:
            virtual bool LoadInternal() override;
            virtual bool PersistInternal(const Aws::Map<Aws::String, Aws::Config::Profile>&) override;

        private:
            Aws::String m_fileName;
            bool m_useProfilePrefix;
        };
    }
}
