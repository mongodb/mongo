
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/client/AWSClient.h>
#include <aws/core/monitoring/CoreMetrics.h>

namespace Aws
{
    namespace Monitoring
    {
        class MonitoringFactory;
        /**
         * Wrapper function of OnRequestStarted defined by all monitoring instances
         */
        Aws::Vector<void*> OnRequestStarted(const Aws::String& serviceName, const Aws::String& requestName,
            const std::shared_ptr<const Aws::Http::HttpRequest>& request);

        /**
         * Wrapper function of OnRequestSucceeded defined by all monitoring instances
         */
        void OnRequestSucceeded(const Aws::String& serviceName, const Aws::String& requestName, const std::shared_ptr<const Aws::Http::HttpRequest>& request,
            const Aws::Client::HttpResponseOutcome& outcome, const CoreMetricsCollection& metricsFromCore, const Aws::Vector<void*>& contexts);

        /**
         * Wrapper function of OnRequestFailed defined by all monitoring instances
         */
        void OnRequestFailed(const Aws::String& serviceName, const Aws::String& requestName, const std::shared_ptr<const Aws::Http::HttpRequest>& request,
            const Aws::Client::HttpResponseOutcome& outcome, const CoreMetricsCollection& metricsFromCore, const Aws::Vector<void*>& contexts);

        /**
         * Wrapper function of OnRequestRetry defined by all monitoring instances
         */
        void OnRequestRetry(const Aws::String& serviceName, const Aws::String& requestName,
            const std::shared_ptr<const Aws::Http::HttpRequest>& request, const Aws::Vector<void*>& contexts);

        /**
         * Wrapper function of OnFinish defined by all monitoring instances
         */
        void OnFinish(const Aws::String& serviceName, const Aws::String& requestName,
            const std::shared_ptr<const Aws::Http::HttpRequest>& request, const Aws::Vector<void*>& contexts);

        typedef std::function<Aws::UniquePtr<MonitoringFactory>()> MonitoringFactoryCreateFunction;

        /**
         * Init monitoring using supplied factories, monitoring can support multiple instances.
         * We will try to (based on config resolution result) create a default client side monitoring listener instance defined in AWS SDK Core module.
         * and create other instances from these factories.
         * This function will be called during Aws::InitAPI call, argument is acquired from Aws::SDKOptions->MonitoringOptions
         */
        AWS_CORE_API void InitMonitoring(const std::vector<MonitoringFactoryCreateFunction>& monitoringFactoryCreateFunctions);

        /**
         * Clean up monitoring related global variables. This should be done first at shutdown, to avoid a race condition in
         * testing whether the global Monitoring instance has been destructed.
         */
        AWS_CORE_API void CleanupMonitoring();

        /**
         * Add monitoring using supplied factories
         */
        AWS_CORE_API void AddMonitoring(const std::vector<MonitoringFactoryCreateFunction>& monitoringFactoryCreateFunctions);

    }
}
