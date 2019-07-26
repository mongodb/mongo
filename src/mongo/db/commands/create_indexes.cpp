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
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/enable_coordinator_for_create_indexes_command_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {

constexpr auto kIndexesFieldName = "indexes"_sd;
constexpr auto kCommandName = "createIndexes"_sd;
constexpr auto kCommitQuorumFieldName = "commitQuorum"_sd;
constexpr auto kIgnoreUnknownIndexOptionsName = "ignoreUnknownIndexOptions"_sd;
constexpr auto kTwoPhaseCommandName = "twoPhaseCreateIndexes"_sd;
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
                    opCtx, parsedIndexSpec, ns, featureCompatibility);
                if (!indexSpecStatus.isOK()) {
                    return indexSpecStatus.getStatus();
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
                   kTwoPhaseCommandName == cmdElemFieldName ||
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
                                                             const BSONObj& cmdObj) {
    if (cmdObj.hasField(kCommitQuorumFieldName)) {
        CommitQuorumOptions commitQuorum;
        uassertStatusOK(commitQuorum.parse(cmdObj.getField(kCommitQuorumFieldName)));
        return commitQuorum;
    } else {
        // Retrieve the default commit quorum if one wasn't passed in, which consists of all
        // data-bearing nodes.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        int numDataBearingMembers = replCoord->getConfig().getNumDataBearingMembers();
        return CommitQuorumOptions(numDataBearingMembers);
    }
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

    const auto metadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    if (!metadata->isSharded())
        return;

    const ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
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
 * Opens or creates database for index creation.
 * On database creation, the lock will be made exclusive.
 */
Database* getOrCreateDatabase(OperationContext* opCtx, StringData dbName, Lock::DBLock* dbLock) {
    auto databaseHolder = DatabaseHolder::get(opCtx);

    if (auto db = databaseHolder->getDb(opCtx, dbName)) {
        return db;
    }

    // Temporarily release the Database lock while holding a Global IX lock. This prevents
    // replication state from changing. Abandon the current snapshot to see changed metadata.
    opCtx->recoveryUnit()->abandonSnapshot();
    dbLock->relockWithMode(MODE_X);
    return databaseHolder->openDb(opCtx, dbName);
}

/**
 * Checks database sharding state. Throws exception on error.
 */
void checkDatabaseShardingState(OperationContext* opCtx, Database* db) {
    auto& dss = DatabaseShardingState::get(db);
    auto dssLock = DatabaseShardingState::DSSLock::lock(opCtx, &dss);
    dss.checkDbVersion(opCtx, dssLock);
}

/**
 * Gets or creates collection to hold indexes.
 * Appends field to command result to indicate if the collection already exists.
 */
Collection* getOrCreateCollection(OperationContext* opCtx,
                                  Database* db,
                                  const NamespaceString& ns,
                                  const BSONObj& cmdObj,
                                  std::string* errmsg,
                                  BSONObjBuilder* result) {
    if (auto collection = db->getCollection(opCtx, ns)) {
        result->appendBool(kCreateCollectionAutomaticallyFieldName, false);
        return collection;
    }

    result->appendBool(kCreateCollectionAutomaticallyFieldName, true);

    if (ViewCatalog::get(db)->lookup(opCtx, ns.ns())) {
        *errmsg = "Cannot create indexes on a view";
        uasserted(ErrorCodes::CommandNotSupportedOnView, *errmsg);
    }

    uassertStatusOK(userAllowedCreateNS(ns.db(), ns.coll()));

    CollectionOptions options;
    options.uuid = UUID::gen();
    return writeConflictRetry(opCtx, kCommandName, ns.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);
        auto collection = db->createCollection(opCtx, ns, options);
        invariant(collection,
                  str::stream() << "Failed to create collection " << ns.ns()
                                << " during index creation: " << redact(cmdObj));
        wunit.commit();
        return collection;
    });
}

bool runCreateIndexes(OperationContext* opCtx,
                      const std::string& dbname,
                      const BSONObj& cmdObj,
                      std::string& errmsg,
                      BSONObjBuilder& result,
                      bool runTwoPhaseBuild) {
    const NamespaceString ns(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
    uassertStatusOK(userAllowedWriteNS(ns));

    // Disallow users from creating new indexes on config.transactions since the sessions code
    // was optimized to not update indexes
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "not allowed to create index on " << ns.ns(),
            ns != NamespaceString::kSessionTransactionsTableNamespace);

    auto specs = uassertStatusOK(
        parseAndValidateIndexSpecs(opCtx, ns, cmdObj, serverGlobalParams.featureCompatibility));

    Status validateTTL = validateTTLOptions(opCtx, cmdObj);
    uassertStatusOK(validateTTL);

    // Do not use AutoGetOrCreateDb because we may relock the database in mode X.
    Lock::DBLock dbLock(opCtx, ns.db(), MODE_IX);
    if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
        uasserted(ErrorCodes::NotMaster,
                  str::stream() << "Not primary while creating indexes in " << ns.ns());
    }

    if (indexesAlreadyExist(opCtx, ns, specs, &result)) {
        return true;
    }

    auto db = getOrCreateDatabase(opCtx, ns.db(), &dbLock);

    checkDatabaseShardingState(opCtx, db);

    opCtx->recoveryUnit()->abandonSnapshot();
    boost::optional<Lock::CollectionLock> exclusiveCollectionLock(
        boost::in_place_init, opCtx, ns, MODE_X);

    // Index builds can safely ignore prepare conflicts and perform writes. On primaries, an
    // exclusive lock in the final drain phase conflicts with prepared transactions.
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    auto collection = getOrCreateCollection(opCtx, db, ns, cmdObj, &errmsg, &result);

    // Use AutoStatsTracker to update Top.
    boost::optional<AutoStatsTracker> statsTracker;
    const boost::optional<int> dbProfilingLevel = boost::none;
    statsTracker.emplace(opCtx,
                         ns,
                         Top::LockType::WriteLocked,
                         AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                         dbProfilingLevel);

    MultiIndexBlock indexer;

    const size_t origSpecsSize = specs.size();
    specs = resolveDefaultsAndRemoveExistingIndexes(opCtx, collection, std::move(specs));

    const int numIndexesBefore = collection->getIndexCatalog()->numIndexesTotal(opCtx);
    if (specs.size() == 0) {
        fillCommandResultWithIndexesAlreadyExistInfo(numIndexesBefore, &result);
        return true;
    }

    result.append("numIndexesBefore", numIndexesBefore);

    if (specs.size() != origSpecsSize) {
        result.append("note", "index already exists");
    }

    for (size_t i = 0; i < specs.size(); i++) {
        const BSONObj& spec = specs[i];
        if (spec["unique"].trueValue()) {
            checkUniqueIndexConstraints(opCtx, ns, spec["key"].Obj());
        }
    }

    // The 'indexer' can throw, so ensure the build cleanup occurs.
    ON_BLOCK_EXIT([&] {
        opCtx->recoveryUnit()->abandonSnapshot();
        if (MONGO_FAIL_POINT(leaveIndexBuildUnfinishedForShutdown)) {
            // Set a flag to leave the persisted index build state intact when cleanUpAfterBuild()
            // is called below. The index build will be found on server startup.
            //
            // Note: this failpoint has two parts, the first to make the index build error and the
            // second to catch it here: the index build must error before commit(), otherwise
            // commit() clears the state.
            indexer.abortWithoutCleanup(opCtx);
        }
        indexer.cleanUpAfterBuild(opCtx, collection);
    });

    std::vector<BSONObj> indexInfoObjs =
        writeConflictRetry(opCtx, kCommandName, ns.ns(), [opCtx, collection, &indexer, &specs] {
            return uassertStatusOK(
                indexer.init(opCtx,
                             collection,
                             specs,
                             MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, collection)));
        });

    // Don't hold an exclusive collection lock during background indexing, so that other readers
    // and writers can proceed during this phase. A BackgroundOperation has been registered on the
    // namespace, so the collection cannot be removed after yielding the lock.
    if (indexer.isBackgroundBuilding()) {
        invariant(BackgroundOperation::inProgForNs(ns));
        opCtx->recoveryUnit()->abandonSnapshot();
        exclusiveCollectionLock.reset();
    }

    // Collection scan and insert into index, followed by a drain of writes received in the
    // background.
    {
        Lock::CollectionLock colLock(opCtx, ns, MODE_IS);
        uassertStatusOK(indexer.insertAllDocumentsInCollection(opCtx, collection));
    }

    if (MONGO_FAIL_POINT(hangAfterIndexBuildDumpsInsertsFromBulk)) {
        log() << "Hanging after dumping inserts from bulk builder";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildDumpsInsertsFromBulk);
    }

    // Perform the first drain while holding an intent lock.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock colLock(opCtx, ns, MODE_IS);

        uassertStatusOK(indexer.drainBackgroundWrites(opCtx));
    }

    if (MONGO_FAIL_POINT(hangAfterIndexBuildFirstDrain)) {
        log() << "Hanging after index build first drain";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildFirstDrain);
    }

    // Perform the second drain while stopping writes on the collection.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock colLock(opCtx, ns, MODE_S);

        uassertStatusOK(indexer.drainBackgroundWrites(opCtx));
    }

    if (MONGO_FAIL_POINT(hangAfterIndexBuildSecondDrain)) {
        log() << "Hanging after index build second drain";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildSecondDrain);
    }

    // Need to get exclusive collection lock back to complete the index build.
    if (indexer.isBackgroundBuilding()) {
        opCtx->recoveryUnit()->abandonSnapshot();
        exclusiveCollectionLock.emplace(opCtx, ns, MODE_X);
    }

    auto databaseHolder = DatabaseHolder::get(opCtx);
    db = databaseHolder->getDb(opCtx, ns.db());
    invariant(db->getCollection(opCtx, ns));

    checkDatabaseShardingState(opCtx, db);

    // Perform the third and final drain while holding the exclusive collection lock.
    uassertStatusOK(indexer.drainBackgroundWrites(opCtx));

    // This is required before completion.
    uassertStatusOK(indexer.checkConstraints(opCtx));

    writeConflictRetry(opCtx, kCommandName, ns.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);

        uassertStatusOK(
            indexer.commit(opCtx,
                           collection,
                           [opCtx, &ns, collection](const BSONObj& spec) {
                               opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                                   opCtx, ns, *(collection->uuid()), spec, false);
                           },
                           MultiIndexBlock::kNoopOnCommitFn));

        wunit.commit();
    });

    result.append("numIndexesAfter", collection->getIndexCatalog()->numIndexesTotal(opCtx));

    return true;
}

bool runCreateIndexesWithCoordinator(OperationContext* opCtx,
                                     const std::string& dbname,
                                     const BSONObj& cmdObj,
                                     std::string& errmsg,
                                     BSONObjBuilder& result,
                                     bool runTwoPhaseBuild) {
    const NamespaceString ns(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
    uassertStatusOK(userAllowedWriteNS(ns));

    // Disallow users from creating new indexes on config.transactions since the sessions code
    // was optimized to not update indexes
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "not allowed to create index on " << ns.ns(),
            ns != NamespaceString::kSessionTransactionsTableNamespace);

    auto specs = uassertStatusOK(
        parseAndValidateIndexSpecs(opCtx, ns, cmdObj, serverGlobalParams.featureCompatibility));
    boost::optional<CommitQuorumOptions> commitQuorum = parseAndGetCommitQuorum(opCtx, cmdObj);

    Status validateTTL = validateTTLOptions(opCtx, cmdObj);
    uassertStatusOK(validateTTL);

    // Preliminary checks before handing control over to IndexBuildsCoordinator:
    // 1) We are in a replication mode that allows for index creation.
    // 2) Check sharding state.
    // 3) Create the collection to hold the index(es) if necessary.
    OptionalCollectionUUID collectionUUID;
    {
        // Do not use AutoGetOrCreateDb because we may relock the database in mode X.
        Lock::DBLock dbLock(opCtx, ns.db(), MODE_IX);
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
            uasserted(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while creating indexes in " << ns.ns());
        }

        if (indexesAlreadyExist(opCtx, ns, specs, &result)) {
            return true;
        }

        auto db = getOrCreateDatabase(opCtx, ns.db(), &dbLock);

        checkDatabaseShardingState(opCtx, db);

        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock collLock(opCtx, ns, MODE_X);

        auto collection = getOrCreateCollection(opCtx, db, ns, cmdObj, &errmsg, &result);
        collectionUUID = collection->uuid();
    }

    // Use AutoStatsTracker to update Top.
    boost::optional<AutoStatsTracker> statsTracker;
    const boost::optional<int> dbProfilingLevel = boost::none;
    statsTracker.emplace(opCtx,
                         ns,
                         Top::LockType::WriteLocked,
                         AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                         dbProfilingLevel);

    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto buildUUID = UUID::gen();
    auto protocol =
        (runTwoPhaseBuild) ? IndexBuildProtocol::kTwoPhase : IndexBuildProtocol::kSinglePhase;
    log() << "Registering index build: " << buildUUID;
    ReplIndexBuildState::IndexCatalogStats stats;
    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions = {commitQuorum};

    try {
        auto buildIndexFuture = uassertStatusOK(indexBuildsCoord->startIndexBuild(
            opCtx, *collectionUUID, specs, buildUUID, protocol, indexBuildOptions));

        auto deadline = opCtx->getDeadline();
        // Date_t::max() means no deadline.
        if (deadline == Date_t::max()) {
            log() << "Waiting for index build to complete: " << buildUUID;
        } else {
            log() << "Waiting for index build to complete: " << buildUUID
                  << " (deadline: " << deadline << ")";
        }

        // Throws on error.
        try {
            stats = buildIndexFuture.get(opCtx);
        } catch (const ExceptionForCat<ErrorCategory::Interruption>& interruptionEx) {
            // It is unclear whether the interruption originated from the current opCtx instance
            // for the createIndexes command or that the IndexBuildsCoordinator task was interrupted
            // independently of this command invocation. We'll defensively abort the index build
            // with the assumption that if the index build was already in the midst of tearing down,
            // this be a no-op.
            log() << "Index build interrupted: " << buildUUID << ": aborting index build.";
            auto abortIndexFuture = indexBuildsCoord->abortIndexBuildByBuildUUID(
                buildUUID,
                str::stream() << "Index build interrupted: " << buildUUID << ": "
                              << interruptionEx.toString());
            log() << "Index build aborted: " << buildUUID << ": "
                  << abortIndexFuture.getNoThrow(opCtx);
            throw;
        }

        log() << "Index build completed: " << buildUUID;
    } catch (DBException& ex) {
        // If the collection is dropped after the initial checks in this function (before the
        // AutoStatsTracker is created), the IndexBuildsCoordinator (either startIndexBuild() or
        // the the task running the index build) may return NamespaceNotFound. This is not
        // considered an error and the command should return success.
        if (ErrorCodes::NamespaceNotFound == ex.code()) {
            log() << "Index build failed: " << buildUUID << ": collection dropped: " << ns;
            return true;
        }

        // All other errors should be forwarded to the caller with index build information included.
        log() << "Index build failed: " << buildUUID << ": " << ex.toStatus();
        ex.addContext(str::stream() << "Index build failed: " << buildUUID << ": Collection " << ns
                                    << " ( " << *collectionUUID << " )");
        throw;
    }

    result.append(kNumIndexesBeforeFieldName, stats.numIndexesBefore);
    result.append(kNumIndexesAfterFieldName, stats.numIndexesAfter);
    if (stats.numIndexesAfter == stats.numIndexesBefore) {
        result.append(kNoteFieldName, "all indexes already exist");
    } else if (stats.numIndexesAfter < stats.numIndexesBefore + int(specs.size())) {
        result.append(kNoteFieldName, "index already exists");
    }

    commitQuorum->append("commitQuorum", &result);

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
        // specs, then we will wait for the build(s) to finish before trying again.
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
        bool shouldLogMessageOnAlreadyBuildingError = true;
        while (true) {
            try {
                if (enableIndexBuildsCoordinatorForCreateIndexesCommand) {
                    return runCreateIndexesWithCoordinator(
                        opCtx, dbname, cmdObj, errmsg, result, false /*two phase build*/);
                }
                return runCreateIndexes(
                    opCtx, dbname, cmdObj, errmsg, result, false /*two phase build*/);
            } catch (const DBException& ex) {
                if (ex.toStatus() != ErrorCodes::IndexBuildAlreadyInProgress) {
                    throw;
                }
                if (shouldLogMessageOnAlreadyBuildingError) {
                    auto bsonElem = cmdObj.getField(kIndexesFieldName);
                    log()
                        << "Received a request to create indexes: '" << bsonElem
                        << "', but found that at least one of the indexes is already being built, '"
                        << ex.toStatus()
                        << "'. This request will wait for the pre-existing index build to finish "
                           "before proceeding.";
                    shouldLogMessageOnAlreadyBuildingError = false;
                }
                // Unset the response fields so we do not write duplicate fields.
                errmsg = "";
                result.resetToEmpty();
                // Reset the snapshot because we have released locks and may reacquire them again
                // later.
                opCtx->recoveryUnit()->abandonSnapshot();
                // This is a bit racy since we are not holding a lock across discovering an
                // in-progress build and starting to listen for completion. It is good enough,
                // however: we can only wait longer than needed, not less.
                BackgroundOperation::waitUntilAnIndexBuildFinishes(opCtx, nss.ns());
            }
        }
    }

} cmdCreateIndex;

/**
 * A temporary duplicate of the createIndexes command that runs two phase index builds for gradual
 * testing purposes. Otherwise, all of the necessary replication changes for the Simultaneous Index
 * Builds project would have to be turned on all at once because so much testing already exists that
 * would break with incremental changes.
 *
 * {twoPhaseCreateIndexes : "bar",
 *  indexes : [ { ns : "test.bar", key : { x : 1 }, name: "x_1" } ],
 *  commitQuorum: "majority" }
 */
class CmdTwoPhaseCreateIndex : public ErrmsgCommandDeprecated {
public:
    CmdTwoPhaseCreateIndex() : ErrmsgCommandDeprecated(kTwoPhaseCommandName) {}

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
        return runCreateIndexesWithCoordinator(
            opCtx, dbname, cmdObj, errmsg, result, true /*two phase build*/);
    }

} cmdTwoPhaseCreateIndex;

}  // namespace
}  // namespace mongo
