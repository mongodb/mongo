/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/config/AWSProfileConfigLoaderBase.h>

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>

namespace Aws
{
    namespace Internal
    {
        class EC2MetadataClient;
    }

    namespace Config
    {
        static const char* const INSTANCE_PROFILE_KEY = "InstanceProfile";

        /**
         * Loads configuration from the EC2 Metadata Service
         */
        class AWS_CORE_API EC2InstanceProfileConfigLoader : public AWSProfileConfigLoader
        {
        public:
            /**
             * If client is nullptr, the default EC2MetadataClient will be created.
             */
            EC2InstanceProfileConfigLoader(const std::shared_ptr<Aws::Internal::EC2MetadataClient>& = nullptr);

            virtual ~EC2InstanceProfileConfigLoader() = default;

        protected:
            virtual bool LoadInternal() override;
        private:
            std::shared_ptr<Aws::Internal::EC2MetadataClient> m_ec2metadataClient;
            int64_t credentialsValidUntilMillis = 0;
            int64_t calculateRetryTime() const;
        };
    }
}
