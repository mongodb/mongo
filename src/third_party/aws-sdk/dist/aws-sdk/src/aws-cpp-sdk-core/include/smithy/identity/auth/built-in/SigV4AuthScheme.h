/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthScheme.h>
#include <smithy/identity/auth/built-in/SigV4AuthSchemeOption.h>

#include <smithy/identity/resolver/built-in/DefaultAwsCredentialIdentityResolver.h>

#include <smithy/identity/identity/AwsCredentialIdentityBase.h>
#include <smithy/identity/signer/built-in/SigV4Signer.h>
#include <smithy/identity/auth/built-in/SigV4AuthScheme.h>


namespace smithy {
    constexpr char SIGV4[] = "aws.auth#sigv4";

    class SigV4AuthScheme : public AuthScheme<AwsCredentialIdentityBase>
    {
    public:
        using AwsCredentialIdentityResolverT = IdentityResolverBase<IdentityT>;
        using AwsCredentialSignerT = AwsSignerBase<IdentityT>;
        using SigV4AuthSchemeParameters = DefaultAuthSchemeResolverParameters;

        //This allows to override the identity resolver
        explicit SigV4AuthScheme(std::shared_ptr<AwsCredentialIdentityResolverT> identityResolver, 
                                 const Aws::String& serviceName,
                                 const Aws::String& region)
            : AuthScheme(SIGV4), 
            m_identityResolver{identityResolver}, 
            m_signer{Aws::MakeShared<AwsSigV4Signer>("SigV4AuthScheme", serviceName, region)}
        {
            assert(m_identityResolver);
            assert(m_signer);
        }

        //delegate constructor
        explicit SigV4AuthScheme(const Aws::String& serviceName,
                                 const Aws::String& region)
            : SigV4AuthScheme(Aws::MakeShared<DefaultAwsCredentialIdentityResolver>("SigV4AuthScheme"),  
                              serviceName,
                              region)
        {
        }

        virtual ~SigV4AuthScheme() = default;

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
