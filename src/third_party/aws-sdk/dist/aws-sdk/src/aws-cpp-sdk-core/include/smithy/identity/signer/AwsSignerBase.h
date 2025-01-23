/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <atomic>
#include <smithy/identity/identity/AwsIdentity.h>

#include <aws/crt/Variant.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/utils/FutureOutcome.h>
#include <aws/core/utils/memory/stl/AWSMap.h>


namespace smithy {
    class AwsSignerCommon {
    public:
        virtual ~AwsSignerCommon() = default;
        /**
         * This handles detection of clock skew between clients and the server and adjusts the clock so that the next request will not
         * fail on the timestamp check.
         */
        virtual void SetClockSkew(const std::chrono::milliseconds& clockSkew) { m_clockSkew = clockSkew; }
        /**
         * Gets the timestamp being used by the signer. This may include a clock skew if a clock skew has been detected.
         */
        virtual Aws::Utils::DateTime GetSigningTimestamp() const { return Aws::Utils::DateTime::Now() + GetClockSkewOffset(); }

    protected:
        virtual std::chrono::milliseconds GetClockSkewOffset() const { return m_clockSkew.load(); }
        std::atomic<std::chrono::milliseconds> m_clockSkew = {};
    };

    template<typename IDENTITY_T>
    class AwsSignerBase : public AwsSignerCommon {
    public:
        using IdentityT = IDENTITY_T;
        static_assert(std::is_base_of<AwsIdentity, IDENTITY_T>::value, "Identity type should inherit AwsIdentity");
        using SigningProperties = Aws::UnorderedMap<Aws::String, Aws::Crt::Variant<Aws::String, bool>>;
        using AdditionalParameters = Aws::UnorderedMap<Aws::String, Aws::Crt::Variant<Aws::String, bool>>;
        using HttpRequest = Aws::Http::HttpRequest;
        using SigningError = Aws::Client::AWSError<Aws::Client::CoreErrors>;
        using SigningFutureOutcome = Aws::Utils::FutureOutcome<std::shared_ptr<HttpRequest>, SigningError>;

        // signer may copy the original httpRequest or create a new one
        virtual SigningFutureOutcome sign(std::shared_ptr<HttpRequest> httpRequest, const IdentityT& identity, SigningProperties properties) = 0;

        virtual ~AwsSignerBase() {};
    };
}