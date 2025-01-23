/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

namespace Aws
{
    namespace Endpoint
    {
        class AWS_CORE_API BuiltInParameters
        {
        public:
            using EndpointParameter = Aws::Endpoint::EndpointParameter;

            BuiltInParameters() = default;
            BuiltInParameters(const BuiltInParameters&) = delete; // avoid accidental copy
            virtual ~BuiltInParameters() = default;

            virtual void SetFromClientConfiguration(const Client::ClientConfiguration& config);
            virtual void SetFromClientConfiguration(const Client::GenericClientConfiguration& config);

            virtual void OverrideEndpoint(const Aws::String& endpoint, const Aws::Http::Scheme& scheme = Aws::Http::Scheme::HTTPS);

            const EndpointParameter& GetParameter(const Aws::String& name) const;
            void SetParameter(EndpointParameter param);
            void SetStringParameter(Aws::String name, Aws::String value);
            void SetBooleanParameter(Aws::String name, bool value);
            void SetStringArrayParameter(Aws::String name, const Aws::Vector<Aws::String>&& value);
            const Aws::Vector<EndpointParameter>& GetAllParameters() const;

        protected:
            Aws::Vector<EndpointParameter> m_params;
        };
    } // namespace Endpoint
} // namespace Aws
