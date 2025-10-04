/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/endpoint/DefaultEndpointProvider.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

#include <aws/iam/IAMEndpointRules.h>


namespace Aws
{
namespace IAM
{
namespace Endpoint
{
using EndpointParameters = Aws::Endpoint::EndpointParameters;
using Aws::Endpoint::EndpointProviderBase;
using Aws::Endpoint::DefaultEndpointProvider;

using IAMClientContextParameters = Aws::Endpoint::ClientContextParameters;

using IAMClientConfiguration = Aws::Client::GenericClientConfiguration;
using IAMBuiltInParameters = Aws::Endpoint::BuiltInParameters;

/**
 * The type for the IAM Client Endpoint Provider.
 * Inherit from this Base class / "Interface" should you want to provide a custom endpoint provider.
 * The SDK must use service-specific type for each service per specification.
 */
using IAMEndpointProviderBase =
    EndpointProviderBase<IAMClientConfiguration, IAMBuiltInParameters, IAMClientContextParameters>;

using IAMDefaultEpProviderBase =
    DefaultEndpointProvider<IAMClientConfiguration, IAMBuiltInParameters, IAMClientContextParameters>;

/**
 * Default endpoint provider used for this service
 */
class AWS_IAM_API IAMEndpointProvider : public IAMDefaultEpProviderBase
{
public:
    using IAMResolveEndpointOutcome = Aws::Endpoint::ResolveEndpointOutcome;

    IAMEndpointProvider()
      : IAMDefaultEpProviderBase(Aws::IAM::IAMEndpointRules::GetRulesBlob(), Aws::IAM::IAMEndpointRules::RulesBlobSize)
    {}

    ~IAMEndpointProvider()
    {
    }
};
} // namespace Endpoint
} // namespace IAM
} // namespace Aws
