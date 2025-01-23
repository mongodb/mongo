/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/AWSClient.h>
#include <aws/core/monitoring/MonitoringInterface.h>
#include <aws/core/monitoring/MonitoringFactory.h>
#include <aws/core/net/SimpleUDP.h>
namespace Aws
{
    namespace Monitoring
    {
        /**
         * Default monitoring implementation definition
         */
        class AWS_CORE_API DefaultMonitoring: public MonitoringInterface
        {
        public:
            const static int DEFAULT_MONITORING_VERSION;
            const static char DEFAULT_CSM_CONFIG_ENABLED[];
            const static char DEFAULT_CSM_CONFIG_CLIENT_ID[];
            const static char DEFAULT_CSM_CONFIG_HOST[];
            const static char DEFAULT_CSM_CONFIG_PORT[];
            const static char DEFAULT_CSM_ENVIRONMENT_VAR_ENABLED[];
            const static char DEFAULT_CSM_ENVIRONMENT_VAR_CLIENT_ID[];
            const static char DEFAULT_CSM_ENVIRONMENT_VAR_HOST[];
            const static char DEFAULT_CSM_ENVIRONMENT_VAR_PORT[];

            /**
             * @brief Construct a default monitoring instance
             * @param clientId, used to identify the application
             * @param host, either the host name or the host ip address (could be ipv4 or ipv6). Note that "localhost" will be treated as host name and address look up will be performed.
             * @param port, used to send collected metric to a local agent listen on this port.
             */
            DefaultMonitoring(const Aws::String& clientId, const Aws::String& host, unsigned short port);

            void* OnRequestStarted(const Aws::String& serviceName, const Aws::String& requestName, const std::shared_ptr<const Aws::Http::HttpRequest>& request) const override;

            void OnRequestSucceeded(const Aws::String& serviceName, const Aws::String& requestName, const std::shared_ptr<const Aws::Http::HttpRequest>& request,
                const Aws::Client::HttpResponseOutcome& outcome, const CoreMetricsCollection& metricsFromCore, void* context) const override;


            void OnRequestFailed(const Aws::String& serviceName, const Aws::String& requestName, const std::shared_ptr<const Aws::Http::HttpRequest>& request,
                const Aws::Client::HttpResponseOutcome& outcome, const CoreMetricsCollection& metricsFromCore, void* context) const override;


            void OnRequestRetry(const Aws::String& serviceName, const Aws::String& requestName,
                const std::shared_ptr<const Aws::Http::HttpRequest>& request, void* context) const override;


            void OnFinish(const Aws::String& serviceName, const Aws::String& requestName,
                const std::shared_ptr<const Aws::Http::HttpRequest>& request, void* context) const override;

            static inline int GetVersion() { return DEFAULT_MONITORING_VERSION; }
        private:
            void CollectAndSendAttemptData(const Aws::String& serviceName, const Aws::String& requestName,
                const std::shared_ptr<const Aws::Http::HttpRequest>& request, const Aws::Client::HttpResponseOutcome& outcome,
                const CoreMetricsCollection& metricsFromCore, void* context) const;

            Aws::Net::SimpleUDP m_udp;
            Aws::String m_clientId;
        };

        class AWS_CORE_API DefaultMonitoringFactory : public MonitoringFactory
        {
        public:
            Aws::UniquePtr<MonitoringInterface> CreateMonitoringInstance() const override;
        };
    } // namespace Monitoring
} // namespace Aws
