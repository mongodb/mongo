
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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog/multi_index_block_impl.h"
#include "mongo/db/client.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildFirstDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildDumpsInsertsFromBulk);

namespace {

const StringData kIndexesFieldName = "indexes"_sd;
const StringData kCommandName = "createIndexes"_sd;

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

    std::vector<BSONObj> indexSpecs;
    for (auto&& cmdElem : cmdObj) {
        auto cmdElemFieldName = cmdElem.fieldNameStringData();

        if (kIndexesFieldName == cmdElemFieldName) {
            if (cmdElem.type() != BSONType::Array) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << kIndexesFieldName
                                      << "' must be an array, but got "
                                      << typeName(cmdElem.type())};
            }

            for (auto&& indexesElem : cmdElem.Obj()) {
                if (indexesElem.type() != BSONType::Object) {
                    return {ErrorCodes::TypeMismatch,
                            str::stream() << "The elements of the '" << kIndexesFieldName
                                          << "' array must be objects, but got "
                                          << typeName(indexesElem.type())};
                }

                auto indexSpecStatus = index_key_validate::validateIndexSpec(
                    opCtx, indexesElem.Obj(), ns, featureCompatibility);
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
        } else if (kCommandName == cmdElemFieldName || isGenericArgument(cmdElemFieldName)) {
            continue;
        } else {
            return {ErrorCodes::BadValue,
                    str::stream() << "Invalid field specified for " << kCommandName << " command: "
                                  << cmdElemFieldName};
        }
    }

    if (!hasIndexesField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << kIndexesFieldName
                              << "' field is a required argument of the "
                              << kCommandName
                              << " command"};
    }

    if (indexSpecs.empty()) {
        return {ErrorCodes::BadValue, "Must specify at least one index to create"};
    }

    return indexSpecs;
}

/**
 * Returns index specifications with attributes (such as "collation") that are inherited from the
 * collection filled in.
 *
 * The returned index specifications will not be equivalent to the ones specified as 'indexSpecs' if
 * any missing attributes were filled in; however, the returned index specifications will match the
 * form stored in the IndexCatalog should any of these indexes already exist.
 */
StatusWith<std::vector<BSONObj>> resolveCollectionDefaultProperties(
    OperationContext* opCtx, const Collection* collection, std::vector<BSONObj> indexSpecs) {
    std::vector<BSONObj> indexSpecsWithDefaults = std::move(indexSpecs);

    for (size_t i = 0, numIndexSpecs = indexSpecsWithDefaults.size(); i < numIndexSpecs; ++i) {
        auto indexSpecStatus = index_key_validate::validateIndexSpecCollation(
            opCtx, indexSpecsWithDefaults[i], collection->getDefaultCollator());
        if (!indexSpecStatus.isOK()) {
            return indexSpecStatus.getStatus();
        }
        auto indexSpec = indexSpecStatus.getValue();

        if (IndexDescriptor::isIdIndexPattern(
                indexSpec[IndexDescriptor::kKeyPatternFieldName].Obj())) {
            std::unique_ptr<CollatorInterface> indexCollator;
            if (auto collationElem = indexSpec[IndexDescriptor::kCollationFieldName]) {
                auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                          ->makeFromBSON(collationElem.Obj());
                // validateIndexSpecCollation() should have checked that the index collation spec is
                // valid.
                invariant(collatorStatus.getStatus());
                indexCollator = std::move(collatorStatus.getValue());
            }
            if (!CollatorInterface::collatorsMatch(collection->getDefaultCollator(),
                                                   indexCollator.get())) {
                return {ErrorCodes::BadValue,
                        str::stream() << "The _id index must have the same collation as the "
                                         "collection. Index collation: "
                                      << (indexCollator.get() ? indexCollator->getSpec().toBSON()
                                                              : CollationSpec::kSimpleSpec)
                                      << ", collection collation: "
                                      << (collection->getDefaultCollator()
                                              ? collection->getDefaultCollator()->getSpec().toBSON()
                                              : CollationSpec::kSimpleSpec)};
            }
        }

        indexSpecsWithDefaults[i] = indexSpec;
    }

    return indexSpecsWithDefaults;
}

/**
 * Returns a vector of index specs with the filled in collection default options and removes any
 * indexes that already exist on the collection. If the returned vector is empty after returning, no
 * new indexes need to be built. Throws on error.
 */
std::vector<BSONObj> resolveDefaultsAndRemoveExistingIndexes(OperationContext* opCtx,
                                                             const Collection* collection,
                                                             std::vector<BSONObj> validatedSpecs) {
    auto swDefaults =
        resolveCollectionDefaultProperties(opCtx, collection, std::move(validatedSpecs));
    uassertStatusOK(swDefaults.getStatus());

    auto specs = std::move(swDefaults.getValue());
    for (size_t i = 0; i < specs.size(); i++) {
        Status status =
            collection->getIndexCatalog()->prepareSpecForCreate(opCtx, specs.at(i)).getStatus();
        if (status.code() == ErrorCodes::IndexAlreadyExists) {
            specs.erase(specs.begin() + i);
            i--;
            continue;
        }
        uassertStatusOK(status);
    }
    return specs;
}

/**
 * { createIndexes : "bar", indexes : [ { ns : "test.bar", key : { x : 1 }, name: "x_1" } ] }
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
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) override {
        const NamespaceString ns(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
        uassertStatusOK(userAllowedWriteNS(ns));

        // Disallow users from creating new indexes on config.transactions since the sessions code
        // was optimized to not update indexes
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "not allowed to create index on " << ns.ns(),
                ns != NamespaceString::kSessionTransactionsTableNamespace);

        auto specs = uassertStatusOK(
            parseAndValidateIndexSpecs(opCtx, ns, cmdObj, serverGlobalParams.featureCompatibility));

        // Do not use AutoGetOrCreateDb because we may relock the database in mode X.
        Lock::DBLock dbLock(opCtx, ns.db(), MODE_IX);
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
            uasserted(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while creating indexes in " << ns.ns());
        }

        const auto indexesAlreadyExist = [&result](int numIndexes) {
            result.append("numIndexesBefore", numIndexes);
            result.append("numIndexesAfter", numIndexes);
            result.append("note", "all indexes already exist");
            return true;
        };

        // Before potentially taking an exclusive database lock, check if all indexes already exist
        // while holding an intent lock. Only continue if new indexes need to be built and the
        // database should be re-locked in exclusive mode.
        {
            AutoGetCollection autoColl(opCtx, ns, MODE_IX);
            if (auto collection = autoColl.getCollection()) {
                auto specsCopy = resolveDefaultsAndRemoveExistingIndexes(opCtx, collection, specs);
                if (specsCopy.size() == 0) {
                    return indexesAlreadyExist(
                        collection->getIndexCatalog()->numIndexesTotal(opCtx));
                }
            }
        }

        // Relocking temporarily releases the Database lock while holding a Global IX lock. This
        // prevents the replication state from changing, but requires abandoning the current
        // snapshot in case indexes change during the period of time where no database lock is held.
        opCtx->recoveryUnit()->abandonSnapshot();
        dbLock.relockWithMode(MODE_X);

        // Allow the strong lock acquisition above to be interrupted, but from this point forward do
        // not allow locks or re-locks to be interrupted.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx, ns.db());
        if (!db) {
            db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, ns.db());
        }
        DatabaseShardingState::get(db).checkDbVersion(opCtx);

        Collection* collection = db->getCollection(opCtx, ns);
        if (collection) {
            result.appendBool("createdCollectionAutomatically", false);
        } else {
            if (db->getViewCatalog()->lookup(opCtx, ns.ns())) {
                errmsg = "Cannot create indexes on a view";
                uasserted(ErrorCodes::CommandNotSupportedOnView, errmsg);
            }

            uassertStatusOK(userAllowedCreateNS(ns.db(), ns.coll()));

            writeConflictRetry(opCtx, kCommandName, ns.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                collection = db->createCollection(opCtx, ns.ns(), CollectionOptions());
                invariant(collection);
                wunit.commit();
            });
            result.appendBool("createdCollectionAutomatically", true);
        }

        // Use AutoStatsTracker to update Top.
        boost::optional<AutoStatsTracker> statsTracker;
        const boost::optional<int> dbProfilingLevel = boost::none;
        statsTracker.emplace(opCtx,
                             ns,
                             Top::LockType::WriteLocked,
                             AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                             dbProfilingLevel);


        MultiIndexBlockImpl indexer(opCtx, collection);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();

        const size_t origSpecsSize = specs.size();
        specs = resolveDefaultsAndRemoveExistingIndexes(opCtx, collection, std::move(specs));

        const int numIndexesBefore = collection->getIndexCatalog()->numIndexesTotal(opCtx);
        if (specs.size() == 0) {
            return indexesAlreadyExist(numIndexesBefore);
        }

        result.append("numIndexesBefore", numIndexesBefore);

        if (specs.size() != origSpecsSize) {
            result.append("note", "index already exists");
        }

        for (size_t i = 0; i < specs.size(); i++) {
            const BSONObj& spec = specs[i];
            if (spec["unique"].trueValue()) {
                _checkUniqueIndexConstraints(opCtx, ns, spec["key"].Obj());
            }
        }

        std::vector<BSONObj> indexInfoObjs =
            writeConflictRetry(opCtx, kCommandName, ns.ns(), [&indexer, &specs] {
                return uassertStatusOK(indexer.init(specs));
            });

        // If we're a background index, replace exclusive db lock with an intent lock, so that
        // other readers and writers can proceed during this phase.
        if (indexer.getBuildInBackground()) {
            opCtx->recoveryUnit()->abandonSnapshot();
            dbLock.relockWithMode(MODE_IX);
        }

        auto relockOnErrorGuard = MakeGuard([&] {
            // Must have exclusive DB lock before we clean up the index build via the
            // destructor of 'indexer'.
            if (indexer.getBuildInBackground()) {
                try {
                    // This function cannot throw today, but we will preemptively prepare for
                    // that day, to avoid data corruption due to lack of index cleanup.
                    opCtx->recoveryUnit()->abandonSnapshot();
                    dbLock.relockWithMode(MODE_X);
                } catch (...) {
                    std::terminate();
                }
            }
        });

        // Collection scan and insert into index, followed by a drain of writes received in the
        // background.
        {
            Lock::CollectionLock colLock(opCtx->lockState(), ns.ns(), MODE_IX);
            uassertStatusOK(indexer.insertAllDocumentsInCollection());
        }

        if (MONGO_FAIL_POINT(hangAfterIndexBuildDumpsInsertsFromBulk)) {
            log() << "Hanging after dumping inserts from bulk builder";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildDumpsInsertsFromBulk);
        }

        // Perform the first drain while holding an intent lock.
        {
            opCtx->recoveryUnit()->abandonSnapshot();
            Lock::CollectionLock colLock(opCtx->lockState(), ns.ns(), MODE_IS);

            LOG(1) << "performing first index build drain";
            uassertStatusOK(indexer.drainBackgroundWritesIfNeeded());
        }

        if (MONGO_FAIL_POINT(hangAfterIndexBuildFirstDrain)) {
            log() << "Hanging after index build first drain";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildFirstDrain);
        }

        // Perform the second drain while stopping writes on the collection.
        {
            opCtx->recoveryUnit()->abandonSnapshot();
            Lock::CollectionLock colLock(opCtx->lockState(), ns.ns(), MODE_S);

            LOG(1) << "performing second index build drain";
            uassertStatusOK(indexer.drainBackgroundWritesIfNeeded());
        }

        if (MONGO_FAIL_POINT(hangAfterIndexBuildSecondDrain)) {
            log() << "Hanging after index build second drain";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildSecondDrain);
        }

        relockOnErrorGuard.Dismiss();

        // Need to return db lock back to exclusive, to complete the index build.
        if (indexer.getBuildInBackground()) {
            opCtx->recoveryUnit()->abandonSnapshot();
            dbLock.relockWithMode(MODE_X);

            Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx, ns.db());
            if (db) {
                DatabaseShardingState::get(db).checkDbVersion(opCtx);
            }

            invariant(db);
            invariant(db->getCollection(opCtx, ns));
        }

        // Perform the third and final drain after releasing a shared lock and reacquiring an
        // exclusive lock on the database.
        LOG(1) << "performing final index build drain";
        uassertStatusOK(indexer.drainBackgroundWritesIfNeeded());

        writeConflictRetry(opCtx, kCommandName, ns.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);

            uassertStatusOK(indexer.commit([opCtx, &ns, collection](const BSONObj& spec) {
                opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                    opCtx, ns, *(collection->uuid()), spec, false);
            }));

            wunit.commit();
        });

        result.append("numIndexesAfter", collection->getIndexCatalog()->numIndexesTotal(opCtx));

        return true;
    }

private:
    static void _checkUniqueIndexConstraints(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const BSONObj& newIdxKey) {
        invariant(opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_X));

        const auto metadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
        if (!metadata->isSharded())
            return;

        const ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
        uassert(ErrorCodes::CannotCreateIndex,
                str::stream() << "cannot create unique index over " << newIdxKey
                              << " with shard key pattern "
                              << shardKeyPattern.toBSON(),
                shardKeyPattern.isUniqueIndexCompatible(newIdxKey));
    }

} cmdCreateIndex;

}  // namespace
}  // namespace mongo
