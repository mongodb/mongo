/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthScheme.h>
#include <smithy/identity/auth/built-in/BearerTokenAuthSchemeOption.h>
#include <smithy/identity/identity/AwsBearerTokenIdentityBase.h>
#include <smithy/identity/resolver/AwsBearerTokenIdentityResolver.h>
#include <smithy/identity/signer/built-in/BearerTokenSigner.h>
namespace smithy
{
class BearerTokenAuthScheme : public AuthScheme<AwsBearerTokenIdentityBase>
{
  public:
    using AwsCredentialIdentityResolverT = IdentityResolverBase<IdentityT>;
    using AwsCredentialSignerT = AwsSignerBase<IdentityT>;
    using BearerTokenAuthSchemeParameters = DefaultAuthSchemeResolverParameters;

    // This allows to override the identity resolver
    explicit BearerTokenAuthScheme(
        std::shared_ptr<AwsCredentialIdentityResolverT> identityResolver,
        const Aws::String &serviceName, const Aws::String &region)
        : AuthScheme("smithy.api#HTTPBearerAuth"),
          m_identityResolver{identityResolver},
          m_signer{Aws::MakeShared<smithy::BearerTokenSigner>(
              "BearerTokenAuthScheme", serviceName, region)}
    {
        assert(m_identityResolver);
        assert(m_signer);
    }

    explicit BearerTokenAuthScheme(const Aws::String &serviceName,
                                   const Aws::String &region)
        : BearerTokenAuthScheme(
              Aws::MakeShared<DefaultAwsBearerTokenIdentityResolver>(
                  "BearerTokenAuthScheme"),
              serviceName, region)
    {
        assert(m_identityResolver);

        assert(m_signer);
    }

    virtual ~BearerTokenAuthScheme() = default;

    std::shared_ptr<AwsCredentialIdentityResolverT> identityResolver() override
    {
        return m_identityResolver;
    }

    std::shared_ptr<AwsCredentialSignerT> signer() override { return m_signer; }

  protected:
    std::shared_ptr<AwsCredentialIdentityResolverT> m_identityResolver;
    std::shared_ptr<AwsCredentialSignerT> m_signer;
};
} // namespace smithy
