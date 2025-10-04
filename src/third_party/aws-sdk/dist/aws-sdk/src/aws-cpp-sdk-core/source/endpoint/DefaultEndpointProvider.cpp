/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/endpoint/DefaultEndpointProvider.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/crt/Api.h>

namespace Aws
{
namespace Endpoint
{

#ifndef AWS_CORE_EXPORTS // Except for Windows DLL
/**
 * Instantiate endpoint providers
 */
template class DefaultEndpointProvider<Aws::Client::GenericClientConfiguration,
            Aws::Endpoint::BuiltInParameters,
            Aws::Endpoint::ClientContextParameters>;
#endif

char CharToDec(const char c)
{
    if(c >= '0' && c <= '9')
        return c - '0';
    if(c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if(c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return 0;
}

Aws::String PercentDecode(Aws::String inputString)
{
    if (inputString.find_first_of("%") == Aws::String::npos)
    {
        return inputString;
    }
    Aws::String result;
    result.reserve(inputString.size());

    bool percentFound = false;
    char firstOctet = 0;
    char secondOctet = 0;
    for(size_t i = 0; i < inputString.size(); ++i)
    {
        const char currentChar = inputString[i];
        if ('%' == currentChar)
        {
            if (percentFound)
            {
                // not percent-encoded string
                result += currentChar;
            }
            percentFound = true;
            continue;
        }

        if (percentFound)
        {
            if ((currentChar >= '0' && currentChar <= '9') ||
                (currentChar >= 'A' && currentChar <= 'F') ||
                (currentChar >= 'a' && currentChar <= 'f'))
            {
                if(!firstOctet)
                {
                    firstOctet = currentChar;
                    continue;
                }
                if(!secondOctet)
                {
                    secondOctet = currentChar;
                    char encodedChar = CharToDec(firstOctet) * 16 + CharToDec(secondOctet);
                    result += encodedChar;

                    percentFound = false;
                    firstOctet = 0;
                    secondOctet = 0;
                    continue;
                }
            } else {
                // Non-percent encoded sequence
                result += '%';
                if(!firstOctet)
                    result += firstOctet;
                result += currentChar;
                percentFound = false;
                firstOctet = 0;
                secondOctet = 0;
                continue;
            }
        }

        if ('+' == currentChar)
        {
            result += ' ';
            continue;
        }
        result += currentChar;
    }
    return result;
}

AWS_CORE_API ResolveEndpointOutcome
ResolveEndpointDefaultImpl(const Aws::Crt::Endpoints::RuleEngine& ruleEngine,
                           const EndpointParameters& builtInParameters,
                           const EndpointParameters& clientContextParameters,
                           const EndpointParameters& endpointParameters)
{
    if(!ruleEngine) {
        AWS_LOGSTREAM_FATAL(DEFAULT_ENDPOINT_PROVIDER_TAG, "Invalid CRT Rule Engine state");
        return ResolveEndpointOutcome(
                Aws::Client::AWSError<Aws::Client::CoreErrors>(
                        Aws::Client::CoreErrors::INTERNAL_FAILURE,
                        "",
                        "CRT Endpoint rule engine is not initialized",
                        false/*retryable*/));
    }

    Aws::Crt::Endpoints::RequestContext crtRequestCtx;

    const Aws::Vector<std::reference_wrapper<const EndpointParameters>> allParameters
            = {std::cref(builtInParameters), std::cref(clientContextParameters), std::cref(endpointParameters)};

    for (const auto& parameterClass : allParameters)
    {
        for(const auto& parameter : parameterClass.get())
        {
            if(EndpointParameter::ParameterType::BOOLEAN == parameter.GetStoredType())
            {
                AWS_LOGSTREAM_TRACE(DEFAULT_ENDPOINT_PROVIDER_TAG, "Endpoint bool eval parameter: " << parameter.GetName() << " = " << parameter.GetBoolValueNoCheck());
                crtRequestCtx.AddBoolean(Aws::Crt::ByteCursorFromCString(parameter.GetName().c_str()), parameter.GetBoolValueNoCheck());
            }
            else if(EndpointParameter::ParameterType::STRING == parameter.GetStoredType())
            {
                AWS_LOGSTREAM_TRACE(DEFAULT_ENDPOINT_PROVIDER_TAG, "Endpoint str eval parameter: " << parameter.GetName() << " = " << parameter.GetStrValueNoCheck());
                crtRequestCtx.AddString(Aws::Crt::ByteCursorFromCString(parameter.GetName().c_str()), Aws::Crt::ByteCursorFromCString(parameter.GetStrValueNoCheck().c_str()));
            }
            else if(EndpointParameter::ParameterType::STRING_ARRAY == parameter.GetStoredType())
            {
                Aws::Crt::Vector<Aws::Crt::ByteCursor> byteCursorArray;
                byteCursorArray.reserve(parameter.GetStrArrayValueNoCheck().size());
                for (const auto &e: parameter.GetStrArrayValueNoCheck())
                {
                    byteCursorArray.emplace_back(Aws::Crt::ByteCursorFromCString(e.c_str()));
                }
                AWS_LOGSTREAM_TRACE(DEFAULT_ENDPOINT_PROVIDER_TAG, 
                "Endpoint str array eval parameter: " << 
                    parameter.GetName() << " = " << 
                    [&parameter]() ->  Aws::String {
                        Aws::OStringStream os;
                        for (const auto &e: parameter.GetStrArrayValueNoCheck())
                        {
                            os<<e<<",";
                        }
                        return os.str();
                    }());
                crtRequestCtx.AddStringArray(Aws::Crt::ByteCursorFromCString(parameter.GetName().c_str()), byteCursorArray);
            }
            else
            {
                return ResolveEndpointOutcome(
                        Aws::Client::AWSError<Aws::Client::CoreErrors>(
                                Aws::Client::CoreErrors::INVALID_QUERY_PARAMETER,
                                "",
                                "Invalid endpoint parameter type for parameter " + parameter.GetName(),
                                false/*retryable*/));
            }
        }
    }

    auto resolved = ruleEngine.Resolve(crtRequestCtx);

    if(resolved.has_value())
    {
        if(resolved->IsError())
        {
            auto crtError = resolved->GetError();
            Aws::String sdkCrtError = crtError ? Aws::String(crtError->begin(), crtError->end()) :
                    "CRT Rule engine resolution resulted in an unknown error";
            return ResolveEndpointOutcome(
                    Aws::Client::AWSError<Aws::Client::CoreErrors>(
                            Aws::Client::CoreErrors::INVALID_PARAMETER_COMBINATION,
                            "",
                            sdkCrtError,
                            false/*retryable*/));
        }
        else if(resolved->IsEndpoint() && resolved->GetUrl())
        {
            Aws::Endpoint::AWSEndpoint endpoint;
            const auto crtUrl = resolved->GetUrl();
            Aws::String sdkCrtUrl = Aws::String(crtUrl->begin(), crtUrl->end());
            AWS_LOGSTREAM_DEBUG(DEFAULT_ENDPOINT_PROVIDER_TAG, "Endpoint rules engine evaluated the endpoint: " << sdkCrtUrl);
            endpoint.SetURL(PercentDecode(std::move(sdkCrtUrl)));

            // Transform attributes
            // Each attribute consist of properties, hence converting CRT properties to SDK attributes
            const auto crtProps = resolved->GetProperties();
            if (crtProps && crtProps->size() > 2) {
                Aws::String sdkCrtProps = crtProps ? Aws::String(crtProps->begin(), crtProps->end()) : "";
                AWS_LOGSTREAM_TRACE(DEFAULT_ENDPOINT_PROVIDER_TAG, "Endpoint rules evaluated props: " << sdkCrtProps);

                Internal::Endpoint::EndpointAttributes epAttributes = Internal::Endpoint::EndpointAttributes::BuildEndpointAttributesFromJson(
                        sdkCrtProps);

                endpoint.SetAttributes(std::move(epAttributes));
            }

            // transform headers
            const auto crtHeaders = resolved->GetHeaders();
            if (crtHeaders)
            {
                Aws::UnorderedMap<Aws::String, Aws::String> sdkHeaders;
                for (const auto& header: *crtHeaders)
                {
                    Aws::String key(header.first.begin(), header.first.end());
                    Aws::String value;
                    for (const auto& crtHeaderValue : header.second)
                    {
                        if(!value.empty()) {
                            value.insert(value.end(), ';');
                        }
                        value.insert(value.end(), crtHeaderValue.begin(), crtHeaderValue.end());
                    }
                    sdkHeaders.emplace(std::move(key), std::move(value));
                }

                endpoint.SetHeaders(std::move(sdkHeaders));
            }

            return ResolveEndpointOutcome(std::move(endpoint));
        }
        else
        {
            return ResolveEndpointOutcome(
                    Aws::Client::AWSError<Aws::Client::CoreErrors>(
                            Aws::Client::CoreErrors::INVALID_QUERY_PARAMETER,
                            "",
                            "Invalid AWS CRT RuleEngine state",
                            false/*retryable*/));
        }
    }

    auto errCode = Aws::Crt::LastError();
    AWS_LOGSTREAM_DEBUG(DEFAULT_ENDPOINT_PROVIDER_TAG, "ERROR: Rule engine has failed to evaluate the endpoint: " << errCode << " " << Aws::Crt::ErrorDebugString(errCode));

    return ResolveEndpointOutcome(
            Aws::Client::AWSError<Aws::Client::CoreErrors>(
                    Aws::Client::CoreErrors::INVALID_QUERY_PARAMETER,
                    "",
                    "Failed to evaluate the endpoint: null output from AWS CRT RuleEngine",
                    false/*retryable*/));

}

} // namespace Endpoint
} // namespace Aws