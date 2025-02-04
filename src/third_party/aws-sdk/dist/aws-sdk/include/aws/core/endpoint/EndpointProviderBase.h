/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/endpoint/AWSEndpoint.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/endpoint/BuiltInParameters.h>
#include <aws/core/endpoint/ClientContextParameters.h>

namespace Aws
{
    namespace Utils
    {
        template< typename R, typename E> class Outcome;
    } // namespace Utils
    namespace Client
    {
        enum class CoreErrors;
    } // namespace CoreErrors

    namespace Endpoint
    {
        using EndpointParameters = Aws::Vector<EndpointParameter>;
        using ResolveEndpointOutcome = Aws::Utils::Outcome<AWSEndpoint, Aws::Client::AWSError<Aws::Client::CoreErrors> >;

        /**
          * EndpointProviderBase is an interface definition that resolves the provided
          *   EndpointParameters to either an Endpoint or an error.
          * This Base class represents a min interface required to be implemented to override an endpoint provider.
          */
        template<typename ClientConfigurationT = Aws::Client::GenericClientConfiguration,
                 typename BuiltInParametersT = Aws::Endpoint::BuiltInParameters,
                 typename ClientContextParametersT = Aws::Endpoint::ClientContextParameters>
        class AWS_CORE_API EndpointProviderBase
        {
        public:
            using BuiltInParameters = BuiltInParametersT;
            using ClientContextParameters = ClientContextParametersT;

            virtual ~EndpointProviderBase() = default;

            /**
             * Initialize client context parameters from a ClientConfiguration
             */
            virtual void InitBuiltInParameters(const ClientConfigurationT& config) = 0;

            /**
             * Function to override endpoint, i.e. to set built-in parameter "AWS::Endpoint"
             */
            virtual void OverrideEndpoint(const Aws::String& endpoint) = 0;

            /**
             * Method for write access to Client Context Parameters (i.e. configurable service-specific parameters)
             */
            virtual ClientContextParametersT& AccessClientContextParameters() = 0;

            /**
             * Method for read-only access to Client Context Parameters (i.e. configurable service-specific parameters)
             */
            virtual const ClientContextParametersT& GetClientContextParameters() const = 0;

            /**
             * The core of the endpoint provider interface.
             */
            virtual ResolveEndpointOutcome ResolveEndpoint(const EndpointParameters& endpointParameters) const = 0;
        };
    } // namespace Endpoint
} // namespace Aws
