/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/core/endpoint/DefaultEndpointProvider.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

#include <aws/s3/S3EndpointRules.h>


namespace Aws
{
namespace S3
{
namespace Endpoint
{
using S3ClientConfiguration = Aws::S3::S3ClientConfiguration;
using EndpointParameters = Aws::Endpoint::EndpointParameters;
using Aws::Endpoint::EndpointProviderBase;
using Aws::Endpoint::DefaultEndpointProvider;

class AWS_S3_API S3ClientContextParameters : public Aws::Endpoint::ClientContextParameters
{
public:
    virtual ~S3ClientContextParameters(){};

    /**
    * Forces this client to use path-style addressing for buckets.
    */
    void SetForcePathStyle(bool value);
    const ClientContextParameters::EndpointParameter& GetForcePathStyle() const;

    /**
    * Disables this client's usage of Multi-Region Access Points.
    */
    void SetDisableMultiRegionAccessPoints(bool value);
    const ClientContextParameters::EndpointParameter& GetDisableMultiRegionAccessPoints() const;

    /**
    * Enables this client to use an ARN's region when constructing an endpoint instead of the client's configured region.
    */
    void SetUseArnRegion(bool value);
    const ClientContextParameters::EndpointParameter& GetUseArnRegion() const;

    /**
    * Enables this client to use S3 Transfer Acceleration endpoints.
    */
    void SetAccelerate(bool value);
    const ClientContextParameters::EndpointParameter& GetAccelerate() const;

    /**
    * Disables this client's usage of Session Auth for S3Express
      buckets and reverts to using conventional SigV4 for those.
    */
    void SetDisableS3ExpressSessionAuth(bool value);
    const ClientContextParameters::EndpointParameter& GetDisableS3ExpressSessionAuth() const;
};

class AWS_S3_API S3BuiltInParameters : public Aws::Endpoint::BuiltInParameters
{
public:
    virtual ~S3BuiltInParameters(){};
    using Aws::Endpoint::BuiltInParameters::SetFromClientConfiguration;
    virtual void SetFromClientConfiguration(const S3ClientConfiguration& config);
};

/**
 * The type for the S3 Client Endpoint Provider.
 * Inherit from this Base class / "Interface" should you want to provide a custom endpoint provider.
 * The SDK must use service-specific type for each service per specification.
 */
using S3EndpointProviderBase =
    EndpointProviderBase<S3ClientConfiguration, S3BuiltInParameters, S3ClientContextParameters>;

using S3DefaultEpProviderBase =
    DefaultEndpointProvider<S3ClientConfiguration, S3BuiltInParameters, S3ClientContextParameters>;

} // namespace Endpoint
} // namespace S3

namespace Endpoint
{
/**
 * Export endpoint provider symbols for Windows DLL, otherwise declare as extern
 */
AWS_S3_EXTERN template class AWS_S3_API
    Aws::Endpoint::EndpointProviderBase<S3::Endpoint::S3ClientConfiguration, S3::Endpoint::S3BuiltInParameters, S3::Endpoint::S3ClientContextParameters>;

AWS_S3_EXTERN template class AWS_S3_API
    Aws::Endpoint::DefaultEndpointProvider<S3::Endpoint::S3ClientConfiguration, S3::Endpoint::S3BuiltInParameters, S3::Endpoint::S3ClientContextParameters>;
} // namespace Endpoint

namespace S3
{
namespace Endpoint
{
/**
 * Default endpoint provider used for this service
 */
class AWS_S3_API S3EndpointProvider : public S3DefaultEpProviderBase
{
public:
    using S3ResolveEndpointOutcome = Aws::Endpoint::ResolveEndpointOutcome;

    S3EndpointProvider()
      : S3DefaultEpProviderBase(Aws::S3::S3EndpointRules::GetRulesBlob(), Aws::S3::S3EndpointRules::RulesBlobSize)
    {}

    ~S3EndpointProvider()
    {
    }
};
} // namespace Endpoint
} // namespace S3
} // namespace Aws
