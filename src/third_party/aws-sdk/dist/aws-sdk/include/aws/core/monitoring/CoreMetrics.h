/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/monitoring/HttpClientMetrics.h>

namespace Aws
{
    namespace Monitoring
    {
        /**
         * Metrics collected from AWS SDK Core include Http Client Metrics and other types of metrics.
         */
        struct AWS_CORE_API CoreMetricsCollection
        {
            /**
             * Metrics collected from underlying http client during execution of a request
             */
            HttpClientMetricsCollection httpClientMetrics;

            // Add Other types of metrics here.
        };
    }
}
