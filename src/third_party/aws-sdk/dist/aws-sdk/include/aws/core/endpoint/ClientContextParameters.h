/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

namespace Aws
{
    namespace Endpoint
    {
        class AWS_CORE_API ClientContextParameters
        {
        public:
            using EndpointParameter = Aws::Endpoint::EndpointParameter;

            ClientContextParameters() = default;
            // avoid accidental copy from endpointProvider::AccessClientContextParameters()
            ClientContextParameters(const ClientContextParameters&) = delete;

            virtual ~ClientContextParameters(){};

            const EndpointParameter& GetParameter(const Aws::String& name) const;
            void SetParameter(EndpointParameter param);
            void SetStringParameter(Aws::String name, Aws::String value);
            void SetBooleanParameter(Aws::String name, bool value);
            void SetStringArrayParameter(Aws::String name, const Aws::Vector<Aws::String>& value);

            const Aws::Vector<EndpointParameter>& GetAllParameters() const;
        protected:
            Aws::Vector<EndpointParameter> m_params;
        };
    } // namespace Endpoint
} // namespace Aws
