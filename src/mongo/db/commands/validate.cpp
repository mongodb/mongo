/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <mutex>
#include <set>
#include <string>


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

// Sets the 'valid' result field to false and returns immediately.
MONGO_FAIL_POINT_DEFINE(validateCmdCollectionNotValid);

namespace {

// Protects the state below.
Mutex _validationMutex;

// Holds the set of full `databaseName.collectionName` namespace strings in progress. Validation
// commands register themselves in this data structure so that subsequent commands on the same
// namespace will wait rather than run in parallel.
std::set<NamespaceString> _validationsInProgress;

// This is waited upon if there is found to already be a validation command running on the targeted
// namespace, as _validationsInProgress would indicate. This is signaled when a validation command
// finishes on any namespace.
stdx::condition_variable _validationNotifier;

/**
 * Creates an aggregation command with a $collStats pipeline that fetches 'storageStats' and
 * 'count'.
 */
BSONObj makeCollStatsCommand(StringData collectionNameOnly) {
    BSONArrayBuilder pipelineBuilder;
    pipelineBuilder << BSON("$collStats"
                            << BSON("storageStats" << BSONObj() << "count" << BSONObj()));
    return BSON("aggregate" << collectionNameOnly << "pipeline" << pipelineBuilder.arr() << "cursor"
                            << BSONObj());
}

/**
 * $collStats never returns more than a single document. If that ever changes in future, validate
 * must invariant so that the handling can be updated, but only invariant in testing environments,
 * never invariant because of debug logging in production situations.
 */
void verifyCommandResponse(const BSONObj& collStatsResult) {
    if (TestingProctor::instance().isEnabled()) {
        invariant(
            !collStatsResult.getObjectField("cursor").isEmpty() &&
                !collStatsResult.getObjectField("cursor").getObjectField("firstBatch").isEmpty(),
            str::stream() << "Expected a cursor to be present in the $collStats results: "
                          << collStatsResult.toString());
        invariant(collStatsResult.getObjectField("cursor").getIntField("id") == 0,
                  str::stream() << "Expected cursor ID to be 0: " << collStatsResult.toString());
    } else {
        uassert(
            7463202,
            str::stream() << "Expected a cursor to be present in the $collStats results: "
                          << collStatsResult.toString(),
            !collStatsResult.getObjectField("cursor").isEmpty() &&
                !collStatsResult.getObjectField("cursor").getObjectField("firstBatch").isEmpty());
        uassert(7463203,
                str::stream() << "Expected cursor ID to be 0: " << collStatsResult.toString(),
                collStatsResult.getObjectField("cursor").getIntField("id") == 0);
    }
}

/**
 * Log the $collStats results for 'nss' to provide additional debug information for validation
 * failures.
 */
void logCollStats(OperationContext* opCtx, const NamespaceString& nss) {
    DBDirectClient client(opCtx);

    BSONObj collStatsResult;
    try {
        // Run $collStats via aggregation.
        client.runCommand(nss.dbName() /* DatabaseName */,
                          makeCollStatsCommand(nss.coll()),
                          collStatsResult /* command return results */);
        // Logging $collStats information is best effort. If the collection doesn't exist, for
        // example, then the $collStats query will fail and the failure reason will be logged.
        uassertStatusOK(getStatusFromWriteCommandReply(collStatsResult));
        verifyCommandResponse(collStatsResult);

        LOGV2_OPTIONS(7463200,
                      logv2::LogTruncation::Disabled,
                      "Corrupt namespace $collStats results",
                      logAttrs(nss),
                      "collStats"_attr =
                          collStatsResult.getObjectField("cursor").getObjectField("firstBatch"));
    } catch (const DBException& ex) {
        // Catch the error so that the validate error does not get overwritten by the attempt to add
        // debug logging.
        LOGV2_WARNING(7463201,
                      "Failed to fetch $collStats for validation error",
                      logAttrs(nss),
                      "error"_attr = ex.toStatus());
    }
}

}  // namespace

/**
 * Example validate command:
 *   {
 *       validate: "collectionNameWithoutTheDBPart",
 *       full: <bool>  // If true, a more thorough (and slower) collection validation is performed.
 *       background: <bool>  // If true, performs validation on the checkpoint of the collection.
 *       checkBSONConformance: <bool> // If true, validates BSON documents more thoroughly.
 *       metadata: <bool>  // If true, performs a faster validation only on metadata.
 *   }
 */
class ValidateCmd : public BasicCommand {
public:
    ValidateCmd() : BasicCommand("validate") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return str::stream()
            << "Validate contents of a namespace by scanning its data structures "
            << "for correctness.\nThis is a slow operation.\n"
            << "\tAdd {full: true} option to do a more thorough check.\n"
            << "\tAdd {background: true} to validate in the background.\n"
            << "\tAdd {repair: true} to run repair mode.\n"
            << "\tAdd {checkBSONConformance: true} to validate BSON documents more thoroughly.\n"
            << "\tAdd {metadata: true} to only check collection metadata.\n"
            << "Cannot specify both {full: true, background: true}.";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool allowsAfterClusterTime(const BSONObj& cmd) const override {
        return false;
    }

    bool canIgnorePrepareConflicts() const override {
        return false;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::validate)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        if (MONGO_unlikely(validateCmdCollectionNotValid.shouldFail())) {
            result.appendBool("valid", false);
            return true;
        }

        SerializationContext reqSerializationCtx = SerializationContext::stateCommandRequest();
        if (auto const expectPrefix = cmdObj.getField("expectPrefix")) {
            reqSerializationCtx.setPrefixState(expectPrefix.boolean());
        }
        if (auto vts = auth::ValidatedTenancyScope::get(opCtx)) {
            // TODO SERVER-82320 we should no longer need to check here once expectPrefix only comes
            // from the unsigned security token.
            if (reqSerializationCtx.getPrefix() == SerializationContext::Prefix::ExcludePrefix) {
                reqSerializationCtx.setPrefixState(vts->isFromAtlasProxy());
            }
        }
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        bool background = cmdObj["background"].trueValue();
        bool logDiagnostics = cmdObj["logDiagnostics"].trueValue();

        const bool fullValidate = cmdObj["full"].trueValue();
        if (background && fullValidate) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with both { background: true }"
                                    << " and { full: true } is not supported.");
        }

        const bool enforceFastCount = cmdObj["enforceFastCount"].trueValue();
        if (background && enforceFastCount) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with both { background: true }"
                                    << " and { enforceFastCount: true } is not supported.");
        }

        const auto rawCheckBSONConformance = cmdObj["checkBSONConformance"];
        const bool checkBSONConformance = rawCheckBSONConformance.trueValue();
        if (rawCheckBSONConformance && !checkBSONConformance &&
            (fullValidate || enforceFastCount)) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Cannot explicitly set 'checkBSONConformance: false' with "
                                       "full validation set.");
        }

        const bool repair = cmdObj["repair"].trueValue();
        if (opCtx->readOnly() && repair) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with { repair: true } in"
                                    << " read-only mode is not supported.");
        }
        if (background && repair) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with both { background: true }"
                                    << " and { repair: true } is not supported.");
        }
        if (enforceFastCount && repair) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with both { enforceFastCount: true }"
                          << " and { repair: true } is not supported.");
        }
        if (checkBSONConformance && repair) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with both { checkBSONConformance: true }"
                          << " and { repair: true } is not supported.");
        }
        repl::ReplicationCoordinator* replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (repair && replCoord->getSettings().isReplSet()) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with { repair: true } can only be"
                          << " performed in standalone mode.");
        }

        const bool metadata = cmdObj["metadata"].trueValue();
        if (metadata &&
            (background || fullValidate || enforceFastCount || checkBSONConformance || repair)) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with { metadata: true } is not"
                                    << " supported with any other options");
        }

        // Background validation uses point-in-time catalog lookups. This requires an instance of
        // the collection at the checkpoint timestamp. Because timestamps aren't used in standalone
        // mode, this prevents the CollectionCatalog from being able to establish the correct
        // collection instance.
        const bool isReplSet = repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
        if (background && !isReplSet) {
            uasserted(ErrorCodes::CommandNotSupported,
                      str::stream() << "Running the validate command with { background: true } "
                                    << "is not supported in standalone mode");
        }

        // The same goes for unreplicated collections, DDL operations are untimestamped.
        if (background && !nss.isReplicated()) {
            uasserted(ErrorCodes::CommandNotSupported,
                      str::stream() << "Running the validate command with { background: true } "
                                    << "is not supported on unreplicated collections");
        }

        if (background && nss.isGlobalIndex()) {
            // TODO SERVER-74209: Reading earlier than the minimum valid snapshot is temporarily not
            // supported for global index collections as it results in extra index entries being
            // detected. This requires further investigation.
            uasserted(ErrorCodes::CommandNotSupported,
                      str::stream() << "Background validation is temporarily disabled"
                                    << " on the global indexes namespace");
        }

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20514,
                  "CMD: validate",
                  logAttrs(nss),
                  "background"_attr = background,
                  "full"_attr = fullValidate,
                  "enforceFastCount"_attr = enforceFastCount,
                  "checkBSONConformance"_attr = checkBSONConformance,
                  "repair"_attr = repair);
        }

        // Only one validation per collection can be in progress, the rest wait.
        {
            stdx::unique_lock<Latch> lock(_validationMutex);
            try {
                opCtx->waitForConditionOrInterrupt(_validationNotifier, lock, [&] {
                    return _validationsInProgress.find(nss) == _validationsInProgress.end();
                });
            } catch (AssertionException& e) {
                CommandHelpers::appendCommandStatusNoThrow(
                    result,
                    {ErrorCodes::CommandFailed,
                     str::stream() << "Exception thrown during validation: " << e.toString()});
                return false;
            }

            _validationsInProgress.insert(nss);
        }

        ON_BLOCK_EXIT([&] {
            stdx::lock_guard<Latch> lock(_validationMutex);
            _validationsInProgress.erase(nss);
            _validationNotifier.notify_all();
        });

        auto mode = [&] {
            if (metadata) {
                return CollectionValidation::ValidateMode::kMetadata;
            }
            if (background) {
                if (checkBSONConformance) {
                    return CollectionValidation::ValidateMode::kBackgroundCheckBSON;
                }
                return CollectionValidation::ValidateMode::kBackground;
            }
            if (enforceFastCount) {
                return CollectionValidation::ValidateMode::kForegroundFullEnforceFastCount;
            }
            if (fullValidate) {
                return CollectionValidation::ValidateMode::kForegroundFull;
            }
            if (checkBSONConformance) {
                return CollectionValidation::ValidateMode::kForegroundCheckBSON;
            }
            return CollectionValidation::ValidateMode::kForeground;
        }();

        auto repairMode = [&] {
            if (opCtx->readOnly()) {
                // On read-only mode we can't make any adjustments.
                return CollectionValidation::RepairMode::kNone;
            }
            switch (mode) {
                case CollectionValidation::ValidateMode::kForeground:
                case CollectionValidation::ValidateMode::kForegroundCheckBSON:
                case CollectionValidation::ValidateMode::kForegroundFull:
                case CollectionValidation::ValidateMode::kForegroundFullIndexOnly:
                    // Foreground validation may not repair data while running as a replica set node
                    // because we do not have timestamps that are required to perform writes.
                    if (replCoord->getSettings().isReplSet()) {
                        return CollectionValidation::RepairMode::kNone;
                    }
                    if (repair) {
                        return CollectionValidation::RepairMode::kFixErrors;
                    }
                    // Foreground validation will adjust multikey metadata by default.
                    return CollectionValidation::RepairMode::kAdjustMultikey;
                default:
                    return CollectionValidation::RepairMode::kNone;
            }
        }();

        if (repair) {
            shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
                PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
        }

        CollectionValidation::AdditionalOptions additionalOptions;
        additionalOptions.enforceTimeseriesBucketsAreAlwaysCompressed =
            cmdObj["enforceTimeseriesBucketsAreAlwaysCompressed"].trueValue();
        additionalOptions.warnOnSchemaValidation = cmdObj["warnOnSchemaValidation"].trueValue();
        additionalOptions.validationVersion = getTestCommandsEnabled()
            ? (ValidationVersion)bsonTestValidationVersion
            : currentValidationVersion;

        ValidateResults validateResults;
        Status status = CollectionValidation::validate(
            opCtx,
            nss,
            mode,
            repairMode,
            additionalOptions,
            &validateResults,
            &result,
            logDiagnostics,
            SerializationContext::stateCommandReply(reqSerializationCtx));
        if (!status.isOK()) {
            return CommandHelpers::appendCommandStatusNoThrow(result, status);
        }

        validateResults.appendToResultObj(&result, /*debugging=*/false);

        if (!validateResults.valid) {
            result.append("advice",
                          "A corrupt namespace has been detected. See "
                          "http://dochub.mongodb.org/core/data-recovery for recovery steps.");
            logCollStats(opCtx, nss);
        }

        return true;
    }
};
MONGO_REGISTER_COMMAND(ValidateCmd).forShard();
}  // namespace mongo
