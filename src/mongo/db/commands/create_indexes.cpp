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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/two_phase_index_build_knobs_gen.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {
// This failpoint simulates a WriteConflictException during createIndexes where the collection is
// implicitly created.
MONGO_FAIL_POINT_DEFINE(createIndexesWriteConflict);

// This failpoint causes createIndexes with an implicit collection creation to hang before the
// collection is created.
MONGO_FAIL_POINT_DEFINE(hangBeforeCreateIndexesCollectionCreate);
MONGO_FAIL_POINT_DEFINE(hangBeforeIndexBuildAbortOnInterrupt);

constexpr auto kIndexesFieldName = "indexes"_sd;
constexpr auto kCommandName = "createIndexes"_sd;
constexpr auto kCommitQuorumFieldName = "commitQuorum"_sd;
constexpr auto kIgnoreUnknownIndexOptionsName = "ignoreUnknownIndexOptions"_sd;
constexpr auto kCreateCollectionAutomaticallyFieldName = "createdCollectionAutomatically"_sd;
constexpr auto kNumIndexesBeforeFieldName = "numIndexesBefore"_sd;
constexpr auto kNumIndexesAfterFieldName = "numIndexesAfter"_sd;
constexpr auto kNoteFieldName = "note"_sd;

/**
 * Parses the index specifications from 'cmdObj', validates them, and returns equivalent index
 * specifications that have any missing attributes filled in. If any index specification is
 * malformed, then an error status is returned.
 */
StatusWith<std::vector<BSONObj>> parseAndValidateIndexSpecs(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const BSONObj& cmdObj,
    const ServerGlobalParams::FeatureCompatibility& featureCompatibility) {
    bool hasIndexesField = false;

    bool ignoreUnknownIndexOptions = false;
    if (cmdObj.hasField(kIgnoreUnknownIndexOptionsName)) {
        auto ignoreUnknownIndexOptionsElement = cmdObj.getField(kIgnoreUnknownIndexOptionsName);
        if (ignoreUnknownIndexOptionsElement.type() != BSONType::Bool) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The field '" << kIgnoreUnknownIndexOptionsName
                                  << "' must be a boolean, but got "
                                  << typeName(ignoreUnknownIndexOptionsElement.type())};
        }
        ignoreUnknownIndexOptions = ignoreUnknownIndexOptionsElement.boolean();
    }

    std::vector<BSONObj> indexSpecs;
    for (auto&& cmdElem : cmdObj) {
        auto cmdElemFieldName = cmdElem.fieldNameStringData();

        if (kIndexesFieldName == cmdElemFieldName) {
            if (cmdElem.type() != BSONType::Array) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << kIndexesFieldName << "' must be an array, but got "
                            << typeName(cmdElem.type())};
            }

            for (auto&& indexesElem : cmdElem.Obj()) {
                if (indexesElem.type() != BSONType::Object) {
                    return {ErrorCodes::TypeMismatch,
                            str::stream() << "The elements of the '" << kIndexesFieldName
                                          << "' array must be objects, but got "
                                          << typeName(indexesElem.type())};
                }

                BSONObj parsedIndexSpec = indexesElem.Obj();
                if (ignoreUnknownIndexOptions) {
                    parsedIndexSpec = index_key_validate::removeUnknownFields(parsedIndexSpec);
                }

                auto indexSpecStatus = index_key_validate::validateIndexSpec(
                    opCtx, parsedIndexSpec, featureCompatibility);
                if (!indexSpecStatus.isOK()) {
                    return indexSpecStatus.getStatus().withContext(
                        str::stream() << "Error in specification " << parsedIndexSpec.toString());
                }
                auto indexSpec = indexSpecStatus.getValue();

                if (IndexDescriptor::isIdIndexPattern(
                        indexSpec[IndexDescriptor::kKeyPatternFieldName].Obj())) {
                    auto status = index_key_validate::validateIdIndexSpec(indexSpec);
                    if (!status.isOK()) {
                        return status;
                    }
                } else if (indexSpec[IndexDescriptor::kIndexNameFieldName].String() == "_id_"_sd) {
                    return {ErrorCodes::BadValue,
                            str::stream() << "The index name '_id_' is reserved for the _id index, "
                                             "which must have key pattern {_id: 1}, found "
                                          << indexSpec[IndexDescriptor::kKeyPatternFieldName]};
                } else if (indexSpec[IndexDescriptor::kIndexNameFieldName].String() == "*"_sd) {
                    // An index named '*' cannot be dropped on its own, because a dropIndex oplog
                    // entry with a '*' as an index name means "drop all indexes in this
                    // collection".  We disallow creation of such indexes to avoid this conflict.
                    return {ErrorCodes::BadValue, "The index name '*' is not valid."};
                }

                indexSpecs.push_back(std::move(indexSpec));
            }

            hasIndexesField = true;
        } else if (kCommandName == cmdElemFieldName || kCommitQuorumFieldName == cmdElemFieldName ||
                   kIgnoreUnknownIndexOptionsName == cmdElemFieldName ||
                   isGenericArgument(cmdElemFieldName)) {
            continue;
        } else {
            return {ErrorCodes::BadValue,
                    str::stream() << "Invalid field specified for " << kCommandName
                                  << " command: " << cmdElemFieldName};
        }
    }

    if (!hasIndexesField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << kIndexesFieldName
                              << "' field is a required argument of the " << kCommandName
                              << " command"};
    }

    if (indexSpecs.empty()) {
        return {ErrorCodes::BadValue, "Must specify at least one index to create"};
    }

    return indexSpecs;
}

void appendFinalIndexFieldsToResult(int numIndexesBefore,
                                    int numIndexesAfter,
                                    BSONObjBuilder& result,
                                    int numSpecs,
                                    boost::optional<CommitQuorumOptions> commitQuorum) {
    result.append(kNumIndexesBeforeFieldName, numIndexesBefore);
    result.append(kNumIndexesAfterFieldName, numIndexesAfter);
    if (numIndexesAfter == numIndexesBefore) {
        result.append(kNoteFieldName, "all indexes already exist");
    } else if (numIndexesAfter < numIndexesBefore + numSpecs) {
        result.append(kNoteFieldName, "index already exists");
    }

    // commitQuorum will be populated only when two phase index build is enabled.
    if (commitQuorum)
        commitQuorum->appendToBuilder(kCommitQuorumFieldName, &result);
}


/**
 * Ensures that the options passed in for TTL indexes are valid.
 */
Status validateTTLOptions(OperationContext* opCtx, const BSONObj& cmdObj) {
    const std::string kExpireAfterSeconds = "expireAfterSeconds";

    const BSONElement& indexes = cmdObj[kIndexesFieldName];
    for (const auto& index : indexes.Array()) {
        BSONObj indexObj = index.Obj();
        if (!indexObj.hasField(kExpireAfterSeconds)) {
            continue;
        }

        const BSONElement expireAfterSecondsElt = indexObj[kExpireAfterSeconds];
        if (!expireAfterSecondsElt.isNumber()) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "TTL index '" << kExpireAfterSeconds
                                  << "' option must be numeric, but received a type of '"
                                  << typeName(expireAfterSecondsElt.type())
                                  << "'. Index spec: " << indexObj};
        }

        if (expireAfterSecondsElt.safeNumberLong() < 0) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "TTL index '" << kExpireAfterSeconds
                                  << "' option cannot be less than 0. Index spec: " << indexObj};
        }

        const std::string tooLargeErr = str::stream()
            << "TTL index '" << kExpireAfterSeconds
            << "' option must be within an acceptable range, try a lower number. Index spec: "
            << indexObj;

        // There are two cases where we can encounter an issue here.
        // The first case is when we try to cast to millseconds from seconds, which could cause an
        // overflow. The second case is where 'expireAfterSeconds' is larger than the current epoch
        // time.
        try {
            auto expireAfterMillis =
                duration_cast<Milliseconds>(Seconds(expireAfterSecondsElt.safeNumberLong()));
            if (expireAfterMillis > Date_t::now().toDurationSinceEpoch()) {
                return {ErrorCodes::CannotCreateIndex, tooLargeErr};
            }
        } catch (const AssertionException&) {
            return {ErrorCodes::CannotCreateIndex, tooLargeErr};
        }

        const BSONObj key = indexObj["key"].Obj();
        if (key.nFields() != 1) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "TTL indexes are single-field indexes, compound indexes do "
                                     "not support TTL. Index spec: "
                                  << indexObj};
        }
    }

    return Status::OK();
}

/**
 * Retrieves the commit quorum from 'cmdObj' if it is present. If it isn't, we provide a default
 * commit quorum, which consists of all the data-bearing nodes.
 */
boost::optional<CommitQuorumOptions> parseAndGetCommitQuorum(OperationContext* opCtx,
                                                             IndexBuildProtocol protocol,
                                                             const BSONObj& cmdObj) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto commitQuorumEnabled = (enableIndexBuildCommitQuorum) ? true : false;

    if (cmdObj.hasField(kCommitQuorumFieldName)) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "Standalones can't specify commitQuorum",
                replCoord->isReplEnabled());
        uassert(ErrorCodes::BadValue,
                str::stream() << "commitQuorum is supported only for two phase index builds with "
                                 "majority commit quorum support enabled ",
                (IndexBuildProtocol::kTwoPhase == protocol && commitQuorumEnabled));
        CommitQuorumOptions commitQuorum;
        uassertStatusOK(commitQuorum.parse(cmdObj.getField(kCommitQuorumFieldName)));
        uassertStatusOK(replCoord->checkIfCommitQuorumCanBeSatisfied(commitQuorum));
        return commitQuorum;
    }

    if (IndexBuildProtocol::kTwoPhase == protocol) {
        // Setting CommitQuorum to 0 will make the index build to opt out of voting proces.
        return (replCoord->isReplEnabled() && commitQuorumEnabled)
            ? CommitQuorumOptions(CommitQuorumOptions::kMajority)
            : CommitQuorumOptions(CommitQuorumOptions::kDisabled);
    }

    return boost::none;
}

/**
 * Returns a vector of index specs with the filled in collection default options and removes any
 * indexes that already exist on the collection -- both ready indexes and in-progress builds. If the
 * returned vector is empty after returning, no new indexes need to be built. Throws on error.
 */
std::vector<BSONObj> resolveDefaultsAndRemoveExistingIndexes(OperationContext* opCtx,
                                                             const Collection* collection,
                                                             std::vector<BSONObj> indexSpecs) {
    auto swDefaults = collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpecs);
    uassertStatusOK(swDefaults.getStatus());

    auto indexCatalog = collection->getIndexCatalog();

    return indexCatalog->removeExistingIndexes(
        opCtx, swDefaults.getValue(), false /*removeIndexBuildsToo*/);
}

void checkUniqueIndexConstraints(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& newIdxKey) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

    const auto collDesc = CollectionShardingState::get(opCtx, nss)->getCollectionDescription();
    if (!collDesc.isSharded())
        return;

    const ShardKeyPattern shardKeyPattern(collDesc.getKeyPattern());
    uassert(ErrorCodes::CannotCreateIndex,
            str::stream() << "cannot create unique index over " << newIdxKey
                          << " with shard key pattern " << shardKeyPattern.toBSON(),
            shardKeyPattern.isUniqueIndexCompatible(newIdxKey));
}

/**
 * Fills in command result with number of indexes when there are no indexes to add.
 */
void fillCommandResultWithIndexesAlreadyExistInfo(int numIndexes, BSONObjBuilder* result) {
    result->append("numIndexesBefore", numIndexes);
    result->append("numIndexesAfter", numIndexes);
    result->append("note", "all indexes already exist");
};

/**
 * Before potentially taking an exclusive database or collection lock, check if all indexes
 * already exist while holding an intent lock.
 *
 * Returns true, after filling in the command result, if the index creation can return early.
 */
bool indexesAlreadyExist(OperationContext* opCtx,
                         const NamespaceString& ns,
                         const std::vector<BSONObj>& specs,
                         BSONObjBuilder* result) {
    AutoGetCollection autoColl(opCtx, ns, MODE_IX);

    auto collection = autoColl.getCollection();
    if (!collection) {
        return false;
    }

    auto specsCopy = resolveDefaultsAndRemoveExistingIndexes(opCtx, collection, specs);
    if (specsCopy.size() > 0) {
        return false;
    }

    auto numIndexes = collection->getIndexCatalog()->numIndexesTotal(opCtx);
    fillCommandResultWithIndexesAlreadyExistInfo(numIndexes, result);

    return true;
}

/**
 * Checks database sharding state. Throws exception on error.
 */
void checkDatabaseShardingState(OperationContext* opCtx, StringData dbName) {
    auto dss = DatabaseShardingState::get(opCtx, dbName);
    auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
    dss->checkDbVersion(opCtx, dssLock);
}

/**
 * Checks collection sharding state. Throws exception on error.
 */
void checkCollectionShardingState(OperationContext* opCtx, const NamespaceString& ns) {
    CollectionShardingState::get(opCtx, ns)->checkShardVersionOrThrow(opCtx);
}

/**
 * Attempts to create indexes in `specs` on a non-existent collection (or empty collection created
 * in the same multi-document transaction) with namespace `ns`. In the former case, the collection
 * is implicitly created.
 * Returns a BSONObj containing fields to be appended to the result of the calling function.
 * `commitQuorum` is passed only to be appended to the result, for completeness. It is otherwise
 * unused.
 * Expects to be run at the end of a larger writeConflictRetry loop.
 */
BSONObj runCreateIndexesOnNewCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& specs,
                                        boost::optional<CommitQuorumOptions> commitQuorum,
                                        bool createCollImplicitly) {
    BSONObjBuilder createResult;

    WriteUnitOfWork wunit(opCtx);

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, ns.db());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            "Cannot create indexes on a view",
            !db || !ViewCatalog::get(db)->lookup(opCtx, ns.ns()));

    if (createCollImplicitly) {
        // We need to create the collection.
        BSONObjBuilder builder;
        builder.append("create", ns.coll());
        CollectionOptions options;
        builder.appendElements(options.toBSON());
        BSONObj idIndexSpec;

        if (MONGO_unlikely(hangBeforeCreateIndexesCollectionCreate.shouldFail())) {
            // Simulate a scenario where a conflicting collection creation occurs
            // mid-index build.
            LOGV2(20437,
                  "Hanging create collection due to failpoint "
                  "'hangBeforeCreateIndexesCollectionCreate'");
            hangBeforeCreateIndexesCollectionCreate.pauseWhileSet();
        }

        auto createStatus =
            createCollection(opCtx, ns.db().toString(), builder.obj().getOwned(), idIndexSpec);

        if (createStatus == ErrorCodes::NamespaceExists) {
            throw WriteConflictException();
        }

        uassertStatusOK(createStatus);
    }

    // By this point, we have exclusive access to our collection, either because we created the
    // collection implicitly as part of createIndexes or because the collection was created earlier
    // in the same multi-document transaction.
    auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, ns);
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx,
                                                                               collection->ns());
    invariant(opCtx->inMultiDocumentTransaction() || createCollImplicitly);

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot create new indexes on non-empty collection " << ns
                          << " in a multi-document transaction.",
            collection->numRecords(opCtx) == 0);

    const int numIndexesBefore = IndexBuildsCoordinator::getNumIndexesTotal(opCtx, collection);
    auto filteredSpecs =
        IndexBuildsCoordinator::prepareSpecListForCreate(opCtx, collection, ns, specs);
    // It's possible for 'filteredSpecs' to be empty if we receive a createIndexes request for the
    // _id index and also create the collection implicitly. By this point, the _id index has already
    // been created, and there is no more work to be done.
    if (!filteredSpecs.empty()) {
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            opCtx, collection->uuid(), filteredSpecs, false);
    }

    const int numIndexesAfter = IndexBuildsCoordinator::getNumIndexesTotal(opCtx, collection);

    if (MONGO_unlikely(createIndexesWriteConflict.shouldFail())) {
        throw WriteConflictException();
    }
    wunit.commit();

    appendFinalIndexFieldsToResult(
        numIndexesBefore, numIndexesAfter, createResult, int(specs.size()), commitQuorum);

    return createResult.obj();
}

bool runCreateIndexesWithCoordinator(OperationContext* opCtx,
                                     const std::string& dbname,
                                     const BSONObj& cmdObj,
                                     std::string& errmsg,
                                     BSONObjBuilder& result) {
    const NamespaceString ns(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));

    // Disallows drops and renames on this namespace.
    BackgroundOperation backgroundOp(ns.ns());

    uassertStatusOK(userAllowedWriteNS(ns));

    // Disallow users from creating new indexes on config.transactions since the sessions code
    // was optimized to not update indexes
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "not allowed to create index on " << ns.ns(),
            ns != NamespaceString::kSessionTransactionsTableNamespace);

    auto specs = uassertStatusOK(
        parseAndValidateIndexSpecs(opCtx, ns, cmdObj, serverGlobalParams.featureCompatibility));
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto protocol = IndexBuildsCoordinator::supportsTwoPhaseIndexBuild() &&
            !replCoord->isOplogDisabledFor(opCtx, ns)
        ? IndexBuildProtocol::kTwoPhase
        : IndexBuildProtocol::kSinglePhase;
    auto commitQuorum = parseAndGetCommitQuorum(opCtx, protocol, cmdObj);

    Status validateTTL = validateTTLOptions(opCtx, cmdObj);
    uassertStatusOK(validateTTL);

    // Preliminary checks before handing control over to IndexBuildsCoordinator:
    // 1) We are in a replication mode that allows for index creation.
    // 2) Check sharding state.
    // 3) Check if we can create the index without handing control to the IndexBuildsCoordinator.
    OptionalCollectionUUID collectionUUID;
    {
        Lock::DBLock dbLock(opCtx, ns.db(), MODE_IX);
        checkDatabaseShardingState(opCtx, ns.db());
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
            uasserted(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while creating indexes in " << ns.ns());
        }

        bool indexExists = writeConflictRetry(opCtx, "createCollectionWithIndexes", ns.ns(), [&] {
            if (indexesAlreadyExist(opCtx, ns, specs, &result)) {
                return true;
            }

            auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, ns);
            if (collection &&
                !UncommittedCollections::get(opCtx).isUncommittedCollection(opCtx, ns)) {
                // The collection exists and was not created in the same multi-document transaction
                // as the createIndexes.
                collectionUUID = collection->uuid();
                result.appendBool(kCreateCollectionAutomaticallyFieldName, false);
                return false;
            }

            bool createCollImplicitly = collection ? false : true;

            auto createIndexesResult = runCreateIndexesOnNewCollection(
                opCtx, ns, specs, commitQuorum, createCollImplicitly);
            // No further sources of WriteConflicts can occur at this point, so it is safe to
            // append elements to `result` inside the writeConflictRetry loop.
            result.appendBool(kCreateCollectionAutomaticallyFieldName, true);
            result.appendElements(createIndexesResult);
            return true;
        });

        if (indexExists) {
            // No need to proceed if the index either already existed or has just been built.
            return true;
        }

        // If the index does not exist by this point, the index build must go through the index
        // builds coordinator and take an exclusive lock. We should not take exclusive locks inside
        // of transactions, so we fail early here if we are inside of a transaction.
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create new indexes on existing collection " << ns
                              << " in a multi-document transaction.",
                !opCtx->inMultiDocumentTransaction());
    }

    // Use AutoStatsTracker to update Top.
    boost::optional<AutoStatsTracker> statsTracker;
    const boost::optional<int> dbProfilingLevel = boost::none;
    statsTracker.emplace(opCtx,
                         ns,
                         Top::LockType::WriteLocked,
                         AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                         dbProfilingLevel);

    auto buildUUID = UUID::gen();
    LOGV2(20438, "Registering index build: {buildUUID}", "buildUUID"_attr = buildUUID);
    ReplIndexBuildState::IndexCatalogStats stats;
    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions = {commitQuorum};

    try {
        auto buildIndexFuture = uassertStatusOK(indexBuildsCoord->startIndexBuild(
            opCtx, dbname, *collectionUUID, specs, buildUUID, protocol, indexBuildOptions));

        auto deadline = opCtx->getDeadline();
        // Date_t::max() means no deadline.
        if (deadline == Date_t::max()) {
            LOGV2(20439,
                  "Waiting for index build to complete: {buildUUID}",
                  "buildUUID"_attr = buildUUID);
        } else {
            LOGV2(20440,
                  "Waiting for index build to complete: {buildUUID} (deadline: {deadline})",
                  "buildUUID"_attr = buildUUID,
                  "deadline"_attr = deadline);
        }

        // Throws on error.
        try {
            stats = buildIndexFuture.get(opCtx);
        } catch (const ExceptionForCat<ErrorCategory::Interruption>& interruptionEx) {
            LOGV2(20441,
                  "Index build interrupted: {buildUUID}: {interruptionEx}",
                  "buildUUID"_attr = buildUUID,
                  "interruptionEx"_attr = interruptionEx);

            hangBeforeIndexBuildAbortOnInterrupt.pauseWhileSet();

            boost::optional<Lock::GlobalLock> globalLock;
            if (IndexBuildProtocol::kTwoPhase == protocol) {
                // If this node is no longer a primary, the index build will continue to run in the
                // background and will complete when this node receives a commitIndexBuild oplog
                // entry from the new primary.
                if (ErrorCodes::InterruptedDueToReplStateChange == interruptionEx.code()) {
                    LOGV2(20442,
                          "Index build continuing in background: {buildUUID}",
                          "buildUUID"_attr = buildUUID);
                    throw;
                }

                // If we are using two-phase index builds and are no longer primary after receiving
                // an interrupt, we cannot replicate an abortIndexBuild oplog entry. Rely on the new
                // primary to finish the index build. Acquire the global lock to check the
                // replication state and to prevent any state transitions from happening while
                // aborting the index build.
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                globalLock.emplace(opCtx, MODE_IS);
                if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
                    uassertStatusOK(
                        {ErrorCodes::NotMaster,
                         str::stream()
                             << "Unable to abort index build because we are no longer primary: "
                             << buildUUID});
                }
            }

            // It is unclear whether the interruption originated from the current opCtx instance
            // for the createIndexes command or that the IndexBuildsCoordinator task was interrupted
            // independently of this command invocation. We'll defensively abort the index build
            // with the assumption that if the index build was already in the midst of tearing down,
            // this be a no-op.
            // Use a null abort timestamp because the index build will generate its own timestamp
            // on cleanup.
            indexBuildsCoord->abortIndexBuildOnError(opCtx, buildUUID, interruptionEx.toStatus());
            LOGV2(20443, "Index build aborted: {buildUUID}", "buildUUID"_attr = buildUUID);

            throw;
        } catch (const ExceptionForCat<ErrorCategory::NotMasterError>& ex) {
            LOGV2(20444,
                  "Index build interrupted due to change in replication state: {buildUUID}: {ex}",
                  "buildUUID"_attr = buildUUID,
                  "ex"_attr = ex);

            // The index build will continue to run in the background and will complete when this
            // node receives a commitIndexBuild oplog entry from the new primary.

            if (IndexBuildProtocol::kTwoPhase == protocol) {
                LOGV2(20445,
                      "Index build continuing in background: {buildUUID}",
                      "buildUUID"_attr = buildUUID);
                throw;
            }

            indexBuildsCoord->abortIndexBuildOnError(opCtx, buildUUID, ex.toStatus());
            LOGV2(20446,
                  "Index build aborted due to NotMaster error: {buildUUID}",
                  "buildUUID"_attr = buildUUID);

            throw;
        }

        LOGV2(20447, "Index build completed: {buildUUID}", "buildUUID"_attr = buildUUID);
    } catch (DBException& ex) {
        // If the collection is dropped after the initial checks in this function (before the
        // AutoStatsTracker is created), the IndexBuildsCoordinator (either startIndexBuild() or
        // the the task running the index build) may return NamespaceNotFound. This is not
        // considered an error and the command should return success.
        if (ErrorCodes::NamespaceNotFound == ex.code()) {
            LOGV2(20448,
                  "Index build failed: {buildUUID}: collection dropped: {ns}",
                  "buildUUID"_attr = buildUUID,
                  "ns"_attr = ns);
            return true;
        }

        // All other errors should be forwarded to the caller with index build information included.
        LOGV2(20449,
              "Index build failed: {buildUUID}: {ex_toStatus}",
              "buildUUID"_attr = buildUUID,
              "ex_toStatus"_attr = ex.toStatus());
        ex.addContext(str::stream() << "Index build failed: " << buildUUID << ": Collection " << ns
                                    << " ( " << *collectionUUID << " )");

        // Set last op on error to provide the client with a specific optime to read the state of
        // the server when the createIndexes command failed.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

        throw;
    }

    // IndexBuildsCoordinator may write the createIndexes oplog entry on a different thread.
    // The current client's last op should be synchronized with the oplog to ensure consistent
    // getLastError results as the previous non-IndexBuildsCoordinator behavior.
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

    appendFinalIndexFieldsToResult(
        stats.numIndexesBefore, stats.numIndexesAfter, result, int(specs.size()), commitQuorum);

    return true;
}

/**
 * { createIndexes : "bar",
 *   indexes : [ { ns : "test.bar", key : { x : 1 }, name: "x_1" } ],
 *   commitQuorum: "majority" }
 */
class CmdCreateIndex : public ErrmsgCommandDeprecated {
public:
    CmdCreateIndex() : ErrmsgCommandDeprecated(kCommandName) {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        ActionSet actions;
        actions.addAction(ActionType::createIndex);
        Privilege p(parseResourcePattern(dbname, cmdObj), actions);
        if (AuthorizationSession::get(client)->isAuthorizedForPrivilege(p))
            return Status::OK();
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        // If we encounter an IndexBuildAlreadyInProgress error for any of the requested index
        // specs, then we will wait for the build(s) to finish before trying again unless we are in
        // a multi-document transaction.
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
        bool shouldLogMessageOnAlreadyBuildingError = true;
        while (true) {
            try {
                return runCreateIndexesWithCoordinator(opCtx, dbname, cmdObj, errmsg, result);
            } catch (const DBException& ex) {
                // We can only wait for an existing index build to finish if we are able to release
                // our locks, in order to allow the existing index build to proceed. We cannot
                // release locks in transactions, so we bypass the below logic in transactions.
                if (ex.toStatus() != ErrorCodes::IndexBuildAlreadyInProgress ||
                    opCtx->inMultiDocumentTransaction()) {
                    throw;
                }
                if (shouldLogMessageOnAlreadyBuildingError) {
                    auto bsonElem = cmdObj.getField(kIndexesFieldName);
                    LOGV2(20450,
                          "Received a request to create indexes: '{bsonElem}', but found that at "
                          "least one of the indexes is already being built, '{ex_toStatus}'. This "
                          "request will wait for the pre-existing index build to finish "
                          "before proceeding.",
                          "bsonElem"_attr = bsonElem,
                          "ex_toStatus"_attr = ex.toStatus());
                    shouldLogMessageOnAlreadyBuildingError = false;
                }
                // Unset the response fields so we do not write duplicate fields.
                errmsg = "";
                result.resetToEmpty();
                // Reset the snapshot because we have released locks and need a fresh snapshot
                // if we reacquire the locks again later.
                opCtx->recoveryUnit()->abandonSnapshot();
                // This is a bit racy since we are not holding a lock across discovering an
                // in-progress build and starting to listen for completion. It is good enough,
                // however: we can only wait longer than needed, not less.
                BackgroundOperation::waitUntilAnIndexBuildFinishes(opCtx, nss.ns());
            }
        }
    }

} cmdCreateIndex;

}  // namespace
}  // namespace mongo
