/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"

#include <functional>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Flags to mark language features with different allowance constraints when API versioning is
 * enabled.
 */
enum class AllowedWithApiStrict {
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
enum class AllowedWithClientType {
    // The stage can be specified in the command request of any client.
    kAny,
    // The stage can be specified in the command request of an internal client only.
    kInternal,
};

// Helper function to get whether a client is internal.
bool isInternalClient(Client* client);

// Use to assert that a feature is allowed only if it is used internally.
void assertAllowedInternalIfRequired(const OperationContext* opCtx,
                                     StringData operatorName,
                                     AllowedWithClientType allowedWithClientType);

/**
 * Asserts that the API parameters in 'apiParameters' are compatible with the restrictions on
 * 'operatorName' given by 'allowedWithApiStrict' and 'allowedWithClientType'. If the operator is
 * allowed 'sometimes', a callback can be provided in 'conditionalCallback' to check if the
 * conditions are met.
 */
void assertLanguageFeatureIsAllowed(
    const OperationContext* opCtx,
    StringData operatorName,
    AllowedWithApiStrict allowedWithApiStrict,
    AllowedWithClientType allowedWithClientType,
    boost::optional<std::function<void(const APIParameters&)>> conditionalCallback = boost::none);

}  // namespace mongo
