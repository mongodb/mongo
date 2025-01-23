/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/endpoint/DefaultEndpointProvider.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

#include <aws/cognito-identity/CognitoIdentityEndpointRules.h>


namespace Aws
{
namespace CognitoIdentity
{
namespace Endpoint
{
using EndpointParameters = Aws::Endpoint::EndpointParameters;
using Aws::Endpoint::EndpointProviderBase;
using Aws::Endpoint::DefaultEndpointProvider;

using CognitoIdentityClientContextParameters = Aws::Endpoint::ClientContextParameters;

using CognitoIdentityClientConfiguration = Aws::Client::GenericClientConfiguration;
using CognitoIdentityBuiltInParameters = Aws::Endpoint::BuiltInParameters;

/**
 * The type for the CognitoIdentity Client Endpoint Provider.
 * Inherit from this Base class / "Interface" should you want to provide a custom endpoint provider.
 * The SDK must use service-specific type for each service per specification.
 */
using CognitoIdentityEndpointProviderBase =
    EndpointProviderBase<CognitoIdentityClientConfiguration, CognitoIdentityBuiltInParameters, CognitoIdentityClientContextParameters>;

using CognitoIdentityDefaultEpProviderBase =
    DefaultEndpointProvider<CognitoIdentityClientConfiguration, CognitoIdentityBuiltInParameters, CognitoIdentityClientContextParameters>;

/**
 * Default endpoint provider used for this service
 */
class AWS_COGNITOIDENTITY_API CognitoIdentityEndpointProvider : public CognitoIdentityDefaultEpProviderBase
{
public:
    using CognitoIdentityResolveEndpointOutcome = Aws::Endpoint::ResolveEndpointOutcome;

    CognitoIdentityEndpointProvider()
      : CognitoIdentityDefaultEpProviderBase(Aws::CognitoIdentity::CognitoIdentityEndpointRules::GetRulesBlob(), Aws::CognitoIdentity::CognitoIdentityEndpointRules::RulesBlobSize)
    {}

    ~CognitoIdentityEndpointProvider()
    {
    }
};
} // namespace Endpoint
} // namespace CognitoIdentity
} // namespace Aws
