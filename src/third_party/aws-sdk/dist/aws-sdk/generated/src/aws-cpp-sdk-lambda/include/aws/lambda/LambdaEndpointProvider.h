/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/endpoint/DefaultEndpointProvider.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

#include <aws/lambda/LambdaEndpointRules.h>


namespace Aws
{
namespace Lambda
{
namespace Endpoint
{
using EndpointParameters = Aws::Endpoint::EndpointParameters;
using Aws::Endpoint::EndpointProviderBase;
using Aws::Endpoint::DefaultEndpointProvider;

using LambdaClientContextParameters = Aws::Endpoint::ClientContextParameters;

using LambdaClientConfiguration = Aws::Client::GenericClientConfiguration;
using LambdaBuiltInParameters = Aws::Endpoint::BuiltInParameters;

/**
 * The type for the Lambda Client Endpoint Provider.
 * Inherit from this Base class / "Interface" should you want to provide a custom endpoint provider.
 * The SDK must use service-specific type for each service per specification.
 */
using LambdaEndpointProviderBase =
    EndpointProviderBase<LambdaClientConfiguration, LambdaBuiltInParameters, LambdaClientContextParameters>;

using LambdaDefaultEpProviderBase =
    DefaultEndpointProvider<LambdaClientConfiguration, LambdaBuiltInParameters, LambdaClientContextParameters>;

/**
 * Default endpoint provider used for this service
 */
class AWS_LAMBDA_API LambdaEndpointProvider : public LambdaDefaultEpProviderBase
{
public:
    using LambdaResolveEndpointOutcome = Aws::Endpoint::ResolveEndpointOutcome;

    LambdaEndpointProvider()
      : LambdaDefaultEpProviderBase(Aws::Lambda::LambdaEndpointRules::GetRulesBlob(), Aws::Lambda::LambdaEndpointRules::RulesBlobSize)
    {}

    ~LambdaEndpointProvider()
    {
    }
};
} // namespace Endpoint
} // namespace Lambda
} // namespace Aws
