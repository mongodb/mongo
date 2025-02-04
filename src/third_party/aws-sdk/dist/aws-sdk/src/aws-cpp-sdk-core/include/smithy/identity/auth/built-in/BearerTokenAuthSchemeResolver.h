/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthSchemeResolverBase.h>
#include <smithy/identity/auth/built-in/BearerTokenAuthSchemeOption.h>

namespace smithy
{
template <typename ServiceAuthSchemeParametersT =
              DefaultAuthSchemeResolverParameters>
class BearerTokenAuthSchemeResolver
    : public AuthSchemeResolverBase<ServiceAuthSchemeParametersT>
{
  public:
    using ServiceAuthSchemeParameters = ServiceAuthSchemeParametersT;
    virtual ~BearerTokenAuthSchemeResolver() = default;

    Aws::Vector<AuthSchemeOption> resolveAuthScheme(
        const ServiceAuthSchemeParameters &identityProperties) override
    {
        AWS_UNREFERENCED_PARAM(identityProperties);
        return {BearerTokenAuthSchemeOption::bearerTokenAuthSchemeOption};
    }
};
} // namespace smithy