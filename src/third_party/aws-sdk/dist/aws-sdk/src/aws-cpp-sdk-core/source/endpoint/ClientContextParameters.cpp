/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/endpoint/ClientContextParameters.h>

namespace Aws
{
namespace Endpoint
{
    const ClientContextParameters::EndpointParameter& ClientContextParameters::GetParameter(const Aws::String& name) const
    {
        const auto foundIt = std::find_if(m_params.begin(), m_params.end(),
                                          [name](const ClientContextParameters::EndpointParameter& item)
                                          {
                                              return item.GetName() == name;
                                          });

        if (foundIt != m_params.end())
        {
            return *foundIt;
        }
        else
        {
            static const ClientContextParameters::EndpointParameter CTX_NOT_FOUND_PARAMETER("PARAMETER_NOT_SET", false, EndpointParameter::ParameterOrigin::CLIENT_CONTEXT);
            return CTX_NOT_FOUND_PARAMETER;
        }
    }

    void ClientContextParameters::SetParameter(EndpointParameter param)
    {
        const auto foundIt = std::find_if(m_params.begin(), m_params.end(),
                                          [param](const ClientContextParameters::EndpointParameter& item)
                                          {
                                              return item.GetName() == param.GetName();
                                          });

        if (foundIt != m_params.end())
        {
            m_params.erase(foundIt);
        }
        m_params.emplace_back(std::move(param));
    }

    void ClientContextParameters::SetStringParameter(Aws::String name, Aws::String value)
    {
        return SetParameter(EndpointParameter(std::move(name), std::move(value), EndpointParameter::ParameterOrigin::CLIENT_CONTEXT));
    }

    void ClientContextParameters::SetBooleanParameter(Aws::String name, bool value)
    {
        return SetParameter(EndpointParameter(std::move(name), value, EndpointParameter::ParameterOrigin::CLIENT_CONTEXT));
    }

    void ClientContextParameters::SetStringArrayParameter(Aws::String name, const Aws::Vector<Aws::String>& value)
    {
        return SetParameter(EndpointParameter(std::move(name), value, EndpointParameter::ParameterOrigin::CLIENT_CONTEXT));
    }

    const Aws::Vector<ClientContextParameters::EndpointParameter>& ClientContextParameters::GetAllParameters() const
    {
        return m_params;
    }
} // namespace Endpoint
} // namespace Aws