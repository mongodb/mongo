/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthSchemeResolverBase.h>
#include <smithy/identity/auth/built-in/SigV4AuthSchemeOption.h>


namespace smithy {
    template<typename ServiceAuthSchemeParametersT = DefaultAuthSchemeResolverParameters>
    class SigV4AuthSchemeResolver : public AuthSchemeResolverBase<ServiceAuthSchemeParametersT>
    {
    public:
        using ServiceAuthSchemeParameters = ServiceAuthSchemeParametersT;
        virtual ~SigV4AuthSchemeResolver() = default;

        Aws::Vector<AuthSchemeOption> resolveAuthScheme(const ServiceAuthSchemeParameters& identityProperties) override
        {
            AWS_UNREFERENCED_PARAM(identityProperties);
            return {SigV4AuthSchemeOption::sigV4AuthSchemeOption};
        }
    };
}