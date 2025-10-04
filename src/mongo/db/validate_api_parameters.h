/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/api_parameters_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

#include "api_parameters.h"

namespace mongo {

class BSONObj;
class Command;
class OperationContext;

/**
 * Validates the provided API parameters, unless not required for the specified command,
 * and throws if the validation fails.
 */
void validateAPIParameters(const CommandInvocation& invocation);
APIParametersFromClient parseAndValidateAPIParameters(const CommandInvocation& invocation);

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
void enforceRequireAPIVersion(OperationContext* opCtx,
                              Command* command,
                              const OpMsgRequest& request);
}  // namespace mongo
