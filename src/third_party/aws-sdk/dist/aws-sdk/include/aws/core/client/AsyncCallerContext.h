/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/memory/stl/AWSString.h>

#include <aws/crt/http/HttpRequestResponse.h>
#include <aws/core/monitoring/MonitoringManager.h>

namespace Aws
{
    namespace Client
    {

        class AWS_CORE_API MonitorContext{

            private:
            mutable Aws::String clientName;
            mutable Aws::String requestName;
            mutable Aws::Vector<void*> contexts;

            public:
            ~MonitorContext() = default;
            MonitorContext() = default;
            MonitorContext(const MonitorContext&) = delete;
            MonitorContext( MonitorContext &&) = delete;
            MonitorContext& operator=(const MonitorContext&) = delete;
            MonitorContext& operator=(MonitorContext&&) = delete;

            void StartMonitorContext(const Aws::String& client, const Aws::String& request, std::shared_ptr<Aws::Http::HttpRequest>& httpRequest) const
            {
                clientName = client;
                requestName = request;
                contexts = Aws::Monitoring::OnRequestStarted(clientName, requestName, httpRequest);
            }

            inline void OnRequestFailed(std::shared_ptr<Aws::Http::HttpRequest>& httpRequest, const Aws::Client::HttpResponseOutcome& outcome) const
            {
                if(!httpRequest)
                {
                    return;
                }
                Aws::Monitoring::CoreMetricsCollection coreMetrics;
                coreMetrics.httpClientMetrics = httpRequest->GetRequestMetrics();
                
                Aws::Monitoring::OnRequestFailed(
                clientName,
                requestName, 
                httpRequest ,
                outcome, 
                coreMetrics, 
                contexts);
                
            }

            inline void OnRequestSucceeded(std::shared_ptr<Aws::Http::HttpRequest> httpRequest, const Aws::Client::HttpResponseOutcome& outcome) const
            {

                if(!httpRequest)
                {
                    return;
                }
                
                Aws::Monitoring::CoreMetricsCollection coreMetrics;
                coreMetrics.httpClientMetrics = httpRequest->GetRequestMetrics();
                
                Aws::Monitoring::OnRequestSucceeded(
                clientName,
                requestName, 
                httpRequest ,
                outcome, 
                coreMetrics, 
                contexts);
            }

            inline void OnRetry(std::shared_ptr<Aws::Http::HttpRequest> httpRequest) const
            {
                if(!httpRequest)
                {
                    return;
                }
                Aws::Monitoring::CoreMetricsCollection coreMetrics;
                coreMetrics.httpClientMetrics = httpRequest->GetRequestMetrics();
                
                Aws::Monitoring::OnRequestRetry(
                clientName, 
                requestName,
                httpRequest, 
                contexts);
            }

            inline void OnFinish(std::shared_ptr<Aws::Http::HttpRequest> httpRequest) const
            {
                if(!httpRequest)
                {
                    return;
                }
                Aws::Monitoring::OnFinish(
                clientName,
                requestName, 
                httpRequest ,
                contexts);
            }

        };

        /**
        * Call-back context for all async client methods. This allows you to pass a context to your callbacks so that you can identify your requests.
        * It is entirely intended that you override this class in-lieu of using a void* for the user context. The base class just gives you the ability to
        * pass a uuid for your context.
        */
        class AWS_CORE_API AsyncCallerContext
        {
        public:

            /**
             * Initializes object with generated UUID
             */
            AsyncCallerContext();

            /**
             * Initializes object with UUID
             */
            AsyncCallerContext(const Aws::String& uuid) : m_uuid(uuid){}

            /**
            * Initializes object with UUID
            */
            AsyncCallerContext(const char* uuid) : m_uuid(uuid) {}

            virtual ~AsyncCallerContext() {}

            /**
             * Gets underlying UUID
             */
            inline const Aws::String& GetUUID() const { return m_uuid; }

            /**
             * Sets underlying UUID
             */
            inline void SetUUID(const Aws::String& value) { m_uuid = value; }

            /**
             * Sets underlying UUID
             */
            inline void SetUUID(const char* value) { m_uuid.assign(value); }

            inline const MonitorContext& GetMonitorContext() const{
                return monitorContext;
            }

        private:
            Aws::String m_uuid;
            mutable MonitorContext monitorContext;
        };
    }
}
