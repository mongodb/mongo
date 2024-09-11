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

#include "mongo/db/validate_api_parameters.h"

#include <memory>
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/api_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

void validateAPIParameters(const CommandInvocation& invocation) {
    auto command = invocation.definition();

    if (command->skipApiVersionCheck()) {
        return;
    }

    auto& genericArgs = invocation.getGenericArguments();

    if (genericArgs.getApiDeprecationErrors() || genericArgs.getApiStrict()) {
        uassert(4886600,
                "Provided apiStrict and/or apiDeprecationErrors without passing apiVersion",
                genericArgs.getApiVersion());
    }

    if (auto apiVersion = genericArgs.getApiVersion(); apiVersion) {
        // Validates the API version.
        getAPIVersion(*apiVersion, acceptApiVersion2);
    }

    if (genericArgs.getApiStrict().value_or(false)) {
        auto& cmdApiVersions = command->apiVersions();
        auto apiVersionFromClient = genericArgs.getApiVersion()->toString();
        bool strictAssert = (cmdApiVersions.find(apiVersionFromClient) != cmdApiVersions.end());
        uassert(ErrorCodes::APIStrictError,
                fmt::format("Provided apiStrict:true, but the command {} is not in API Version {}. "
                            "Information on supported commands and migrations in API Version {} "
                            "can be found at https://dochub.mongodb.org/core/manual-versioned-api.",
                            command->getName(),
                            apiVersionFromClient,
                            apiVersionFromClient),
                strictAssert);
        if (invocation.definition()->getReadWriteType() == Command::ReadWriteType::kWrite) {
            for (auto& ns : invocation.allNamespaces()) {
                uassert(ErrorCodes::APIStrictError,
                        fmt::format("Provided apiStrict:true, but the command {} attempts to "
                                    "write to system.js.",
                                    invocation.definition()->getName()),
                        !ns.isSystemDotJavascript());
            }
        }
    }

    if (genericArgs.getApiDeprecationErrors().value_or(false)) {
        auto& cmdDepApiVersions = command->deprecatedApiVersions();
        auto apiVersionFromClient = genericArgs.getApiVersion()->toString();
        bool deprecationAssert =
            (cmdDepApiVersions.find(apiVersionFromClient) == cmdDepApiVersions.end());
        uassert(ErrorCodes::APIDeprecationError,
                fmt::format("Provided apiDeprecationErrors:true, but the command {} is deprecated "
                            "in API Version {}.",
                            command->getName(),
                            apiVersionFromClient),
                deprecationAssert);
    }
}

APIParametersFromClient parseAndValidateAPIParameters(const CommandInvocation& invocation) {
    validateAPIParameters(invocation);
    APIParametersFromClient apiParams;
    apiParams.setApiStrict(invocation.getGenericArguments().getApiStrict());
    apiParams.setApiVersion(invocation.getGenericArguments().getApiVersion());
    apiParams.setApiDeprecationErrors(invocation.getGenericArguments().getApiDeprecationErrors());
    return apiParams;
}

void enforceRequireAPIVersion(OperationContext* opCtx, Command* command) {
    auto client = opCtx->getClient();
    const bool isInternalThreadOrClient = [&] {
        // No Transport Session indicates this command is internally sourced.
        if (!client->session()) {
            return true;
        }

        // Pre-auth commands are allowed to trust the {hello: 1, internalClient: ...}
        // claim without auth validation.
        if (command && !command->requiresAuth() &&
            client->isPossiblyUnauthenticatedInternalClient()) {
            return true;
        }

        // Otherwise, only authenticated client authorized to claim internality are
        // treated as internal.
        return client->isInternalClient();
    }();

    if (gRequireApiVersion.load() && !client->isInDirectClient() && !isInternalThreadOrClient) {
        // TODO This log aims to give more diagnostics for BF-32357, and will be removed when
        // the BF has been resolved.
        if (getTestCommandsEnabled()) {
            LOGV2(9451900,
                  "apiVersion parameter requirement check failed.",
                  "clientDesc"_attr = client->desc(),
                  "session"_attr = client->hasRemote() ? client->session()->toBSON().toString()
                                                       : "No session exists.",
                  "isPossiblyUnauthenticatedInternalClient"_attr =
                      client->isPossiblyUnauthenticatedInternalClient(),
                  "commandPassed"_attr = !!command,
                  "commandRequiresAuth"_attr = command ? command->requiresAuth() : true);
        }
        uassert(
            498870,
            "The apiVersion parameter is required, please configure your MongoClient's API version",
            APIParameters::get(opCtx).getParamsPassed());
    }
}
}  // namespace mongo
