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


#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/validate_db_metadata_common.h"
#include "mongo/db/commands/validate_db_metadata_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ValidateDBMetadataCmd : public TypedCommand<ValidateDBMetadataCmd> {
    using _TypedCommandInvocationBase =
        typename TypedCommand<ValidateDBMetadataCmd>::InvocationBase;

public:
    using Request = ValidateDBMetadataCommandRequest;
    using Reply = ValidateDBMetadataCommandReply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const {
        // The db metadata maybe stale or incorrect while the node is in recovery mode, so we
        // disallow the command.
        return false;
    }

    class Invocation : public _TypedCommandInvocationBase {
    public:
        using _TypedCommandInvocationBase::_TypedCommandInvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }
        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            assertUserCanRunValidate(opCtx, request());
        }

        Reply typedRun(OperationContext* opCtx) {
            auto shardResponses = scatterGatherUnversionedTargetAllShards(
                opCtx,
                request().getDbName(),
                applyReadWriteConcern(
                    opCtx,
                    this,
                    CommandHelpers::filterCommandRequestForPassthrough(unparsedRequest().body)),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent);

            bool hasMoreErrors = false;
            std::vector<ErrorReplyElement> apiVersionErrorsToReturn;
            ValidateDBMetadataSizeTracker sizeTracker;
            for (auto&& shardRes : shardResponses) {
                // Re-throw errors from any shard.
                auto shardOutput = uassertStatusOK(shardRes.swResponse).data;
                uassertStatusOK(getStatusFromCommandResult(shardOutput));

                auto apiVersionErrors =
                    shardOutput[ValidateDBMetadataCommandReply::kApiVersionErrorsFieldName];
                tassert(5287400,
                        "The 'apiVersionErrors' field returned from shards should be an array ",
                        apiVersionErrors && apiVersionErrors.type() == Array);
                for (auto&& error : apiVersionErrors.Array()) {
                    tassert(5287401,
                            "The array element in 'apiVersionErrors' should be object",
                            error.type() == Object);
                    ErrorReplyElement apiVersionError = ErrorReplyElement::parse(
                        IDLParserContext("ErrorReplyElement"), error.Obj());

                    // Ensure that the final output doesn't exceed max BSON size.
                    apiVersionError.setShard(StringData(shardRes.shardId.toString()));
                    if (!sizeTracker.incrementAndCheckOverflow(apiVersionError)) {
                        hasMoreErrors = true;
                        break;
                    }

                    apiVersionErrorsToReturn.push_back(std::move(apiVersionError));
                }
                if (hasMoreErrors ||
                    shardOutput.getField(ValidateDBMetadataCommandReply::kHasMoreErrorsFieldName)
                        .trueValue()) {
                    hasMoreErrors = true;
                    break;
                }
            }

            ValidateDBMetadataCommandReply reply;
            reply.setApiVersionErrors(std::move(apiVersionErrorsToReturn));
            if (hasMoreErrors) {
                reply.setHasMoreErrors(true);
            }

            return reply;
        }
    };
} validateDBMetadataCmd;
}  // namespace
}  // namespace mongo
