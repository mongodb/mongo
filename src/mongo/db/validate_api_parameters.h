// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/api_parameters_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {

class BSONObj;
class Command;
class OperationContext;

/**
 * Validates the provided API parameters, unless not required for the specified command,
 * and throws if the validation fails.
 */
void validateAPIParameters(const CommandInvocation& invocation);
[[MONGO_MOD_PUBLIC]] APIParametersFromClient parseAndValidateAPIParameters(
    const CommandInvocation& invocation);

template <typename StringType>
int getAPIVersion(StringType apiVersion, bool allowTestVersion) {
    if (MONGO_likely(apiVersion == "1")) {
        return 1;
    } else if (apiVersion == "2") {
        uassert(ErrorCodes::APIVersionError, "Cannot accept API version 2", allowTestVersion);
        return 2;
    } else {
        uasserted(ErrorCodes::APIVersionError, "API version must be \"1\"");
    }
}

/**
 * If the server parameter "requireApiVersion" is set, enforce it. This check is bypassed for
 * "hello" commands from internal clients.
 */
[[MONGO_MOD_PUBLIC]] void enforceRequireAPIVersion(OperationContext* opCtx,
                                                   Command* command,
                                                   const OpMsgRequest& request);
}  // namespace mongo
