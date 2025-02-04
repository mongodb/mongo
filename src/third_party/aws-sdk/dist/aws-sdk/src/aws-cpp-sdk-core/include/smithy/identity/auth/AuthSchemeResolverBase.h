/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthSchemeOption.h>
#include <smithy/identity/signer/AwsSignerBase.h>

#include <aws/crt/Variant.h>
#include <aws/core/utils/memory/stl/AWSMap.h>

namespace smithy {
    /**
    * A base interface for code-generated interfaces for passing in the data required for determining the
    * authentication scheme. By default, this only includes the operation name.
    */
    class DefaultAuthSchemeResolverParameters
    {
    public:
        Aws::String serviceName;
        Aws::String operation;
        Aws::Crt::Optional<Aws::String> region;

        Aws::UnorderedMap<Aws::String, Aws::Crt::Variant<Aws::String, 
                bool, 
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy, 
                Aws::Auth::AWSSigningAlgorithm > > additionalProperties;

    };

    template<typename ServiceAuthSchemeParametersT = DefaultAuthSchemeResolverParameters>
    class AuthSchemeResolverBase
    {
    public:
        using ServiceAuthSchemeParameters = ServiceAuthSchemeParametersT;

        virtual ~AuthSchemeResolverBase() = default;
        // AuthScheme Resolver returns a list of AuthSchemeOptions for some reason, according to the SRA...
        virtual Aws::Vector<AuthSchemeOption> resolveAuthScheme(const ServiceAuthSchemeParameters& identityProperties) = 0;
    };
}