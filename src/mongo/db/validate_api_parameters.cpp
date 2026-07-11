// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate_api_parameters.h"

#include "mongo/base/error_codes.h"
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
#include "mongo/platform/atomic.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
        auto apiVersionFromClient = std::string{*genericArgs.getApiVersion()};
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
        auto apiVersionFromClient = std::string{*genericArgs.getApiVersion()};
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

void enforceRequireAPIVersion(OperationContext* opCtx,
                              Command* command,
                              const OpMsgRequest& request) {
    auto client = opCtx->getClient();

    if (gRequireApiVersion.load() && !client->isInDirectClient()) {
        const bool isInternalThreadOrClient = [&] {
            // No Transport Session indicates this command is internally sourced.
            if (!client->session()) {
                return true;
            }

            if (command && !command->requiresAuth()) {
                // Pre-auth commands are allowed to trust the {hello: 1, internalClient: ...}
                // claim without auth validation.
                if (client->isPossiblyUnauthenticatedInternalClient() ||
                    (command->handshakeRole() == BasicCommand::HandshakeRole::kHello &&
                     request.body["internalClient"].trueValue())) {
                    return true;
                }
            }

            // Otherwise, only authenticated client authorized to claim internality are
            // treated as internal.
            return client->isInternalClient();
        }();
        if (!isInternalThreadOrClient) {
            uassert(498870,
                    "The apiVersion parameter is required, please configure your MongoClient's API "
                    "version",
                    APIParameters::get(opCtx).getParamsPassed());
        }
    }
}
}  // namespace mongo
