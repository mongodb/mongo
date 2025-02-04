/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/endpoint/AWSPartitions.h>
#include <aws/core/endpoint/EndpointProviderBase.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/endpoint/ClientContextParameters.h>
#include <aws/core/endpoint/BuiltInParameters.h>
#include <aws/core/utils/memory/stl/AWSArray.h>

#include <aws/crt/endpoints/RuleEngine.h>

#include <aws/core/utils/Outcome.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include "aws/core/utils/logging/LogMacros.h"

namespace Aws
{
    namespace Endpoint
    {
        static const char DEFAULT_ENDPOINT_PROVIDER_TAG[] = "Aws::Endpoint::DefaultEndpointProvider";

        /**
         * Default template implementation for endpoint resolution
         * @param ruleEngine
         * @param builtInParameters
         * @param clientContextParameters
         * @param endpointParameters
         * @return
         */
        AWS_CORE_API ResolveEndpointOutcome
        ResolveEndpointDefaultImpl(const Aws::Crt::Endpoints::RuleEngine& ruleEngine,
                                   const EndpointParameters& builtInParameters,
                                   const EndpointParameters& clientContextParameters,
                                   const EndpointParameters& endpointParameters);

        /**
         * Default endpoint provider template used in this SDK.
         */
        template<typename ClientConfigurationT = Aws::Client::GenericClientConfiguration,
                 typename BuiltInParametersT = Aws::Endpoint::BuiltInParameters,
                 typename ClientContextParametersT = Aws::Endpoint::ClientContextParameters>
        class AWS_CORE_API DefaultEndpointProvider : public EndpointProviderBase<ClientConfigurationT, BuiltInParametersT, ClientContextParametersT>
        {
        public:
            DefaultEndpointProvider(const char* endpointRulesBlob, const size_t endpointRulesBlobSz)
                : m_crtRuleEngine(Aws::Crt::ByteCursorFromArray((const uint8_t*) endpointRulesBlob, endpointRulesBlobSz),
                                  Aws::Crt::ByteCursorFromArray((const uint8_t*) AWSPartitions::GetPartitionsBlob(), AWSPartitions::PartitionsBlobSize))
            {
                if(!m_crtRuleEngine) {
                    AWS_LOGSTREAM_FATAL(DEFAULT_ENDPOINT_PROVIDER_TAG, "Invalid CRT Rule Engine state");
                }
            }

            virtual ~DefaultEndpointProvider()
            {
            }

            void InitBuiltInParameters(const ClientConfigurationT& config) override
            {
                m_builtInParameters.SetFromClientConfiguration(config);
            }

            /**
             * Default implementation of the ResolveEndpoint
             */
            ResolveEndpointOutcome ResolveEndpoint(const EndpointParameters& endpointParameters) const override
            {
                auto ResolveEndpointDefaultImpl = Aws::Endpoint::ResolveEndpointDefaultImpl;
                return ResolveEndpointDefaultImpl(m_crtRuleEngine, m_builtInParameters.GetAllParameters(), m_clientContextParameters.GetAllParameters(), endpointParameters);
            };

            const ClientContextParametersT& GetClientContextParameters() const override
            {
                return m_clientContextParameters;
            }
            ClientContextParametersT& AccessClientContextParameters() override
            {
                return m_clientContextParameters;
            }

            const BuiltInParametersT& GetBuiltInParameters() const
            {
                return m_builtInParameters;
            }
            BuiltInParametersT& AccessBuiltInParameters()
            {
                return m_builtInParameters;
            }

            void OverrideEndpoint(const Aws::String& endpoint) override
            {
                m_builtInParameters.OverrideEndpoint(endpoint);
            }

        protected:
            /* Crt RuleEngine evaluator built using the service's Rule engine */
            Aws::Crt::Endpoints::RuleEngine m_crtRuleEngine;

            /* Also known as a configurable parameters defined by the AWS Service in their c2j/smithy model definition */
            ClientContextParametersT m_clientContextParameters;

            /* Also known as parameters on the ClientConfiguration in this SDK */
            BuiltInParametersT m_builtInParameters;
        };

        /**
         * Export endpoint provider symbols for Windows DLL, otherwise declare as extern
         */
        AWS_CORE_EXTERN template class AWS_CORE_API DefaultEndpointProvider<Aws::Client::GenericClientConfiguration,
            Aws::Endpoint::BuiltInParameters,
            Aws::Endpoint::ClientContextParameters>;
    } // namespace Endpoint
} // namespace Aws
