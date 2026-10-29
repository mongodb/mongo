// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Flags to mark language features with different allowance constraints when API versioning is
 * enabled.
 */
enum class [[MONGO_MOD_PUBLIC]] AllowedWithApiStrict {
    // The stage is always allowed in the pipeline regardless of API versions.
    kAlways,
    // This stage can be allowed in a stable API version, depending on the parameters.
    kConditionally,
    // The stage is allowed only for internal client when 'apiStrict' is set to true.
    kInternal,
    // The stage is never allowed in API version '1' when 'apiStrict' is set to true.
    kNeverInVersion1
};

/**
 * Determines the type of client which is permitted to use a particular stage in its command
 * request. Ensures that only internal clients are permitted to send or deserialize certain
 * stages.
 */
enum class [[MONGO_MOD_PUBLIC]] AllowedWithClientType {
    // The stage can be specified in the command request of any client.
    kAny,
    // The stage can be specified in the command request of an internal client only.
    kInternal,
};

// Helper function to get whether a client is internal.
bool isInternalClient(Client* client);

// Use to assert that a feature is allowed only if it is used internally.
void assertAllowedInternalIfRequired(const OperationContext* opCtx,
                                     std::string_view operatorName,
                                     AllowedWithClientType allowedWithClientType);

/**
 * Asserts that the API parameters in 'apiParameters' are compatible with the restrictions on
 * 'operatorName' given by 'allowedWithApiStrict' and 'allowedWithClientType'. If the operator is
 * allowed 'sometimes', a callback can be provided in 'conditionalCallback' to check if the
 * conditions are met.
 */
void assertLanguageFeatureIsAllowed(
    const OperationContext* opCtx,
    std::string_view operatorName,
    AllowedWithApiStrict allowedWithApiStrict,
    AllowedWithClientType allowedWithClientType,
    boost::optional<std::function<void(const APIParameters&)>> conditionalCallback = boost::none);

}  // namespace mongo
