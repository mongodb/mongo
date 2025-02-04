/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthScheme.h>
#include <smithy/identity/auth/built-in/SigV4aAuthSchemeOption.h>

#include <smithy/identity/resolver/built-in/DefaultAwsCredentialIdentityResolver.h>

#include <smithy/identity/identity/AwsCredentialIdentityBase.h>
#include <smithy/identity/signer/built-in/SigV4aSigner.h>


namespace smithy {
    constexpr char SIGV4A[] = "aws.auth#sigv4a";
    

    class SigV4aAuthScheme : public AuthScheme<AwsCredentialIdentityBase>
    {
    public:
        using AwsCredentialIdentityResolverT = IdentityResolverBase<IdentityT>;
        using AwsCredentialSignerT = AwsSignerBase<IdentityT>;
        using SigV4aAuthSchemeParameters = DefaultAuthSchemeResolverParameters;

        //This allows to override the identity resolver
        explicit SigV4aAuthScheme(std::shared_ptr<AwsCredentialIdentityResolverT> identityResolver, 
                                  const Aws::String& serviceName,
                                  const Aws::String& region)
            : AuthScheme(SIGV4A), 
            m_identityResolver{identityResolver}, 
            m_signer{Aws::MakeShared<AwsSigV4aSigner>("SigV4aAuthScheme", serviceName, region)}
        {
            assert(m_identityResolver);
            assert(m_signer);
        }

        explicit SigV4aAuthScheme(const Aws::String& serviceName,
                                  const Aws::String& region)
            : SigV4aAuthScheme(Aws::MakeShared<DefaultAwsCredentialIdentityResolver>("SigV4aAuthScheme"), serviceName, region)
        {
            assert(m_identityResolver);

            assert(m_signer);
        }

        virtual ~SigV4aAuthScheme() = default;

        std::shared_ptr<AwsCredentialIdentityResolverT> identityResolver() override
        {
            return m_identityResolver;
        }

        std::shared_ptr<AwsCredentialSignerT> signer() override
        {
            return m_signer;
        }
    protected:
        std::shared_ptr<AwsCredentialIdentityResolverT> m_identityResolver;
        std::shared_ptr<AwsCredentialSignerT> m_signer;
    };
}
