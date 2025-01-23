/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/endpoint/DefaultEndpointProvider.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

#include <aws/kinesis/KinesisEndpointRules.h>


namespace Aws
{
namespace Kinesis
{
namespace Endpoint
{
using EndpointParameters = Aws::Endpoint::EndpointParameters;
using Aws::Endpoint::EndpointProviderBase;
using Aws::Endpoint::DefaultEndpointProvider;

using KinesisClientContextParameters = Aws::Endpoint::ClientContextParameters;

using KinesisClientConfiguration = Aws::Client::GenericClientConfiguration;
using KinesisBuiltInParameters = Aws::Endpoint::BuiltInParameters;

/**
 * The type for the Kinesis Client Endpoint Provider.
 * Inherit from this Base class / "Interface" should you want to provide a custom endpoint provider.
 * The SDK must use service-specific type for each service per specification.
 */
using KinesisEndpointProviderBase =
    EndpointProviderBase<KinesisClientConfiguration, KinesisBuiltInParameters, KinesisClientContextParameters>;

using KinesisDefaultEpProviderBase =
    DefaultEndpointProvider<KinesisClientConfiguration, KinesisBuiltInParameters, KinesisClientContextParameters>;

/**
 * Default endpoint provider used for this service
 */
class AWS_KINESIS_API KinesisEndpointProvider : public KinesisDefaultEpProviderBase
{
public:
    using KinesisResolveEndpointOutcome = Aws::Endpoint::ResolveEndpointOutcome;

    KinesisEndpointProvider()
      : KinesisDefaultEpProviderBase(Aws::Kinesis::KinesisEndpointRules::GetRulesBlob(), Aws::Kinesis::KinesisEndpointRules::RulesBlobSize)
    {}

    ~KinesisEndpointProvider()
    {
    }
};
} // namespace Endpoint
} // namespace Kinesis
} // namespace Aws
