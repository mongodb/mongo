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
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <set>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

// Sets the 'valid' result field to false and returns immediately.
MONGO_FAIL_POINT_DEFINE(validateCmdCollectionNotValid);

namespace {

// Protects the state below.
stdx::mutex _validationMutex;

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

boost::optional<std::string> getConfigOverrideOrThrow(const BSONElement& raw) {
    if (!raw) {
        return boost::none;
    }
    StringData chosenConfig = raw.valueStringDataSafe();
    // Only a specific subset of valid configurations are allowlisted here. This is mostly to avoid
    // having complex logic to parse/sanitize the user-chosen configuration string.
    static const char* allowed[] = {
        "",
        "dump_address",
        "dump_blocks",
        "dump_layout",
        "dump_pages",
        "dump_tree_shape",
    };
    static const char** allowedEnd = allowed + std::size(allowed);
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Unrecognized configuration string " << chosenConfig,
            std::find(allowed, allowedEnd, chosenConfig) != allowedEnd);
    return {raw.str()};
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
            << "\tAdd {fixMultikey: true} to fix the multi key.\n"
            << "\tAdd {checkBSONConformance: true} to validate BSON documents more thoroughly.\n"
            << "\tAdd {metadata: true} to only check collection metadata.\n"
            << "\tAdd {collHash: true} to generate a full hash of the documents in the "
               "collection.\n"
            << "Cannot specify both {full: true, background: true}.";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
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
             BSONObjBuilder& result) override {
        if (MONGO_unlikely(validateCmdCollectionNotValid.shouldFail())) {
            result.appendBool("valid", false);
            return true;
        }

        SerializationContext reqSerializationCtx = SerializationContext::stateCommandRequest();
        if (auto vts = auth::ValidatedTenancyScope::get(opCtx)) {
            reqSerializationCtx.setPrefixState(vts->isFromAtlasProxy());
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

        const auto rawFixMultikey = cmdObj["fixMultikey"];
        const bool fixMultikey = cmdObj["fixMultikey"].trueValue();
        if (fixMultikey && replCoord->getSettings().isReplSet()) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with { fixMultikey: true } can only be"
                          << " performed in standalone mode.");
        }
        if (rawFixMultikey && !fixMultikey && repair) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with both { fixMultikey: false }"
                          << " and { repair: true } is not supported.");
        }

        const bool metadata = cmdObj["metadata"].trueValue();
        if (metadata &&
            (background || fullValidate || enforceFastCount || checkBSONConformance || repair)) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with { metadata: true } is not"
                                    << " supported with any other options");
        }

        const auto rawConfigOverride = cmdObj["wiredtigerVerifyConfigurationOverride"];
        if (rawConfigOverride && !(fullValidate || enforceFastCount)) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Overriding the verify configuration is only supported with "
                                       "full validation set.");
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

        // collHash parameter.
        const bool collHash = cmdObj["collHash"].trueValue();

        // hashPrefixes parameter.
        const auto rawHashPrefixes = cmdObj["hashPrefixes"];
        boost::optional<std::vector<BSONElement>> hashPrefixes = boost::none;
        if (rawHashPrefixes) {
            hashPrefixes = rawHashPrefixes.Array();
        }
        if (rawHashPrefixes && replCoord->getSettings().isReplSet()) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with { hashPrefixes: [] } can only be"
                          << " performed in standalone mode.");
        }
        if (rawHashPrefixes && !collHash) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with { hashPrefixes: [] }"
                                    << " requires {collHash: true}.");
        }

        // unhash parameter.
        const auto rawUnhash = cmdObj["unhash"];
        boost::optional<std::vector<BSONElement>> unhash = boost::none;
        if (rawUnhash) {
            unhash = rawUnhash.Array();
        }
        if (rawUnhash && replCoord->getSettings().isReplSet()) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with { unhash: [] } can only be"
                          << " performed in standalone mode.");
        }
        if (rawUnhash && !collHash) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "Running the validate command with { unhash: [] }"
                                    << " requires {collHash: true}.");
        }
        if (rawUnhash && rawHashPrefixes) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with { unhash: [] } cannot be done with"
                          << " {hashPrefixes: []}.");
        }
        if (rawUnhash && unhash.get().empty()) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Running the validate command with { unhash: [] } cannot be done with"
                          << " an empty array provided.");
        }

        auto validateMode = [&] {
            if (metadata) {
                return CollectionValidation::ValidateMode::kMetadata;
            }
            if (hashPrefixes) {
                return CollectionValidation::ValidateMode::kHashDrillDown;
            }
            if (collHash) {
                return CollectionValidation::ValidateMode::kCollectionHash;
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
            switch (validateMode) {
                case CollectionValidation::ValidateMode::kForeground:
                case CollectionValidation::ValidateMode::kCollectionHash:
                case CollectionValidation::ValidateMode::kHashDrillDown:
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
                    if (fixMultikey) {
                        return CollectionValidation::RepairMode::kAdjustMultikey;
                    }
                    return CollectionValidation::RepairMode::kNone;
                default:
                    return CollectionValidation::RepairMode::kNone;
            }
        }();

        CollectionValidation::ValidationOptions options(
            validateMode,
            repairMode,
            logDiagnostics,
            getTestCommandsEnabled() ? (ValidationVersion)bsonTestValidationVersion
                                     : currentValidationVersion,
            getConfigOverrideOrThrow(rawConfigOverride),
            hashPrefixes,
            unhash);

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20514,
                  "CMD: validate",
                  logAttrs(nss),
                  "background"_attr = options.isBackground(),
                  "full"_attr = options.isFullValidation(),
                  "extended"_attr = options.isExtendedValidation(),
                  "enforceFastCount"_attr = options.enforceFastCountRequested(),
                  "checkBSONConformance"_attr = options.isBSONConformanceValidation(),
                  "fixMultiKey"_attr = options.adjustMultikey(),
                  "repair"_attr = options.fixErrors(),
                  "wiredtigerVerifyConfigurationOverride"_attr =
                      options.verifyConfigurationOverride());
        }

        // Only one validation per collection can be in progress, the rest wait.
        {
            stdx::unique_lock<stdx::mutex> lock(_validationMutex);
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
            stdx::lock_guard<stdx::mutex> lock(_validationMutex);
            _validationsInProgress.erase(nss);
            _validationNotifier.notify_all();
        });

        if (repair) {
            shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
                PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
        }

        ValidateResults validateResults;
        Status status =
            CollectionValidation::validate(opCtx, nss, std::move(options), &validateResults);
        if (!status.isOK()) {
            return CommandHelpers::appendCommandStatusNoThrow(result, status);
        }

        validateResults.appendToResultObj(
            &result,
            /*debugging=*/false,
            SerializationContext::stateCommandReply(reqSerializationCtx));

        if (!validateResults.isValid()) {
            result.append("advice",
                          "A corrupt namespace has been detected. See "
                          "http://dochub.mongodb.org/core/data-recovery for recovery steps.");
            // Errors stemming from structural damage of the index or record store make it unsafe to
            // open a cursor.
            bool indexHasStructuralDamage =
                std::any_of(validateResults.getIndexResultsMap().begin(),
                            validateResults.getIndexResultsMap().end(),
                            [](auto& indexPair) { return indexPair.second.hasStructuralDamage(); });

            if (validateResults.hasStructuralDamage() || indexHasStructuralDamage) {
                LOGV2_WARNING(
                    9635600,
                    "Skipping logCollStats due to structural damage detected in collection");
                return true;
            }
            logCollStats(opCtx, nss);
        }

        return true;
    }
};
MONGO_REGISTER_COMMAND(ValidateCmd).forShard();
}  // namespace mongo
