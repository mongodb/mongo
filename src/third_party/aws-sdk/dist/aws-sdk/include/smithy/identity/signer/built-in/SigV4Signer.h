/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/signer/AwsSignerBase.h>
#include <smithy/identity/identity/AwsCredentialIdentityBase.h>

#include <aws/core/auth/signer/AWSAuthV4Signer.h>

#include <aws/core/auth/AWSCredentials.h>

namespace smithy {
    /**
     * A smithy SigV4 signer wrapper on top of legacy SDK SigV4 signer
     * TODO: refactor into own signer using smithy design
     */
    class AwsSigV4Signer : public AwsSignerBase<AwsCredentialIdentityBase> {
        
    public:
        using SigV4AuthSchemeParameters = DefaultAuthSchemeResolverParameters;
        explicit AwsSigV4Signer(const Aws::String& serviceName, const Aws::String& region)
            : m_serviceName(serviceName),
              m_region(region),
              legacySigner(nullptr, serviceName.c_str(), region, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Always)
        {
        }

        SigningFutureOutcome sign(std::shared_ptr<HttpRequest> httpRequest, const AwsCredentialIdentityBase& identity, SigningProperties properties) override
        {
            const auto legacyCreds = [&identity]() -> Aws::Auth::AWSCredentials {
                if(identity.sessionToken().has_value() && identity.expiration().has_value())
                {
                    return {identity.accessKeyId(), identity.secretAccessKey(), *identity.sessionToken(), *identity.expiration()};
                }
                if(identity.sessionToken().has_value())
                {
                    return {identity.accessKeyId(), identity.secretAccessKey(), *identity.sessionToken()};
                }
                return {identity.accessKeyId(), identity.secretAccessKey()};
            }();


            
            auto signPayloadIt = properties.find("SignPayload");
            bool signPayload = signPayloadIt != properties.end() ? signPayloadIt->second.get<Aws::String>() == "true" : false;

            assert(httpRequest);
            bool success = legacySigner.SignRequestWithCreds(*httpRequest, legacyCreds, m_region.c_str(), m_serviceName.c_str(), signPayload);
            if (success)
            {
                return SigningFutureOutcome(std::move(httpRequest));
            }
            return SigningError(Aws::Client::CoreErrors::MEMORY_ALLOCATION, "", "Failed to sign the request with sigv4", false);
        }

        virtual ~AwsSigV4Signer() {};
    protected:
        Aws::String m_serviceName;
        Aws::String m_region;
        Aws::Client::AWSAuthV4Signer legacySigner;
    };
}
