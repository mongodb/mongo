/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/Core_EXPORTS.h>

namespace Aws
{
    namespace Monitoring
    {
        class MonitoringInterface;

        /**
         * Factory to create monitoring instance.
         */
        class AWS_CORE_API MonitoringFactory
        {
        public:
            virtual ~MonitoringFactory() = default;
            virtual Aws::UniquePtr<MonitoringInterface> CreateMonitoringInstance() const = 0;
        };

    } // namespace Monitoring
} // namespace Aws
