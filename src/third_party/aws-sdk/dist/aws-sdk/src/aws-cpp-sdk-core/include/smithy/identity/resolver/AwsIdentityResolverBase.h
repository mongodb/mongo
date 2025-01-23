/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/crt/Optional.h>
#include <aws/crt/Variant.h>

#include <aws/core/client/CoreErrors.h>
#include <aws/core/utils/FutureOutcome.h>

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>

#include <aws/core/utils/DateTime.h>

namespace smithy {
    template<typename IDENTITY_T>
    class IdentityResolverBase {
    public:
        using IdentityT = IDENTITY_T;

        virtual ~IdentityResolverBase(){};

        using IdentityProperties = Aws::UnorderedMap<Aws::String, Aws::Crt::Variant<Aws::String, bool>>;
        // IdentityResolvers are asynchronous.
        using ResolveIdentityFutureOutcome = Aws::Utils::FutureOutcome<Aws::UniquePtr<IdentityT>, Aws::Client::AWSError<Aws::Client::CoreErrors>>;
        using AdditionalParameters = Aws::UnorderedMap<Aws::String, Aws::Crt::Variant<Aws::String, bool>>;

        // Each Identity has one or more identity resolvers that are able to load the customer’s
        // Identity. An identity resolver might load the identity from a remote service (e.g. STS), a local
        // service (e.g. IMDS), local disk (e.g. a configuration file) or local memory (e.g. environment variables).
        virtual ResolveIdentityFutureOutcome getIdentity(const IdentityProperties& identityProperties, const AdditionalParameters& additionalParameters) = 0;
    };
}