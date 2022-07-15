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


#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <map>
#include <set>
#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

class DBHashCmd : public ErrmsgCommandDeprecated {
public:
    DBHashCmd() : ErrmsgCommandDeprecated("dbHash", "dbhash") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool allowsAfterClusterTime(const BSONObj& cmd) const override {
        return false;
    }

    bool canIgnorePrepareConflicts() const override {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const final {

        static const Status kReadConcernNotSupported{ErrorCodes::InvalidOptions,
                                                     "read concern not supported"};
        static const Status kDefaultReadConcernNotPermitted{ErrorCodes::InvalidOptions,
                                                            "default read concern not permitted"};
        // The dbHash command only supports local and snapshot read concern. Additionally, snapshot
        // read concern is only supported if test commands are enabled.
        return {{level != repl::ReadConcernLevel::kLocalReadConcern &&
                     (!getTestCommandsEnabled() ||
                      level != repl::ReadConcernLevel::kSnapshotReadConcern),
                 kReadConcernNotSupported},
                kDefaultReadConcernNotPermitted};
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::dbHash);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    virtual bool errmsgRun(OperationContext* opCtx,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           std::string& errmsg,
                           BSONObjBuilder& result) {
        Timer timer;

        std::set<std::string> desiredCollections;
        if (cmdObj["collections"].type() == Array) {
            BSONObjIterator i(cmdObj["collections"].Obj());
            while (i.more()) {
                BSONElement e = i.next();
                if (e.type() != String) {
                    errmsg = "collections entries have to be strings";
                    return false;
                }
                desiredCollections.insert(e.String());
            }
        }

        const std::string ns = parseNs(dbname, cmdObj);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid db name: " << ns,
                NamespaceString::validDBName(ns, NamespaceString::DollarInDbNameBehavior::Allow));

        if (auto elem = cmdObj["$_internalReadAtClusterTime"]) {
            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalReadAtClusterTime' option is only supported when testing"
                    " commands are enabled",
                    getTestCommandsEnabled());

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalReadAtClusterTime' option is only supported when replication is"
                    " enabled",
                    replCoord->isReplEnabled());

            uassert(ErrorCodes::TypeMismatch,
                    "The '$_internalReadAtClusterTime' option must be a Timestamp",
                    elem.type() == BSONType::bsonTimestamp);

            auto targetClusterTime = elem.timestamp();

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "$_internalReadAtClusterTime value must not be a null"
                                     " timestamp.",
                    !targetClusterTime.isNull());

            // We aren't holding the global lock in intent mode, so it is possible after comparing
            // 'targetClusterTime' to 'lastAppliedOpTime' for the last applied opTime to go
            // backwards or for the term to change due to replication rollback. This isn't an actual
            // concern because the testing infrastructure won't use the $_internalReadAtClusterTime
            // option in any test suite where rollback is expected to occur.
            auto lastAppliedOpTime = replCoord->getMyLastAppliedOpTime();

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "$_internalReadAtClusterTime value must not be greater"
                                     " than the last applied opTime. Requested clusterTime: "
                                  << targetClusterTime.toString()
                                  << "; last applied opTime: " << lastAppliedOpTime.toString(),
                    lastAppliedOpTime.getTimestamp() >= targetClusterTime);

            // We aren't holding the global lock in intent mode, so it is possible for the global
            // storage engine to have been destructed already as a result of the server shutting
            // down. This isn't an actual concern because the testing infrastructure won't use the
            // $_internalReadAtClusterTime option in any test suite where clean shutdown is expected
            // to occur concurrently with tests running.
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            auto allDurableTime = storageEngine->getAllDurableTimestamp();
            invariant(!allDurableTime.isNull());

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "$_internalReadAtClusterTime value must not be greater"
                                     " than the all_durable timestamp. Requested clusterTime: "
                                  << targetClusterTime.toString()
                                  << "; all_durable timestamp: " << allDurableTime.toString(),
                    allDurableTime >= targetClusterTime);

            // The $_internalReadAtClusterTime option causes any storage-layer cursors created
            // during plan execution to read from a consistent snapshot of data at the supplied
            // clusterTime, even across yields.
            opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                          targetClusterTime);

            // The $_internalReadAtClusterTime option also causes any storage-layer cursors created
            // during plan execution to block on prepared transactions. Since the dbhash command
            // ignores prepare conflicts by default, change the behavior.
            opCtx->recoveryUnit()->setPrepareConflictBehavior(PrepareConflictBehavior::kEnforce);
        }

        // We lock the entire database in S-mode in order to ensure that the contents will not
        // change for the snapshot.
        auto lockMode = LockMode::MODE_S;
        boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock> shouldNotConflictBlock;
        if (opCtx->recoveryUnit()->getTimestampReadSource() ==
            RecoveryUnit::ReadSource::kProvided) {
            // However, if we are performing a read at a timestamp, then we only need to lock the
            // database in intent mode and then collection in intent mode as well to ensure that
            // none of the collections get dropped.
            lockMode = LockMode::MODE_IS;

            // Additionally, if we are performing a read at a timestamp, then we allow oplog
            // application to proceed concurrently with the dbHash command. This is done
            // to ensure a prepare conflict is able to eventually be resolved by processing a
            // later commitTransaction or abortTransaction oplog entry.
            shouldNotConflictBlock.emplace(opCtx->lockState());
        }

        // TODO SERVER-67459 Pass dbName obj directly
        AutoGetDb autoDb(opCtx, DatabaseName(boost::none, ns), lockMode);
        Database* db = autoDb.getDb();

        result.append("host", prettyHostName());

        md5_state_t globalState;
        md5_init(&globalState);

        std::map<std::string, std::string> collectionToHashMap;
        std::map<std::string, UUID> collectionToUUIDMap;
        std::set<std::string> cappedCollectionSet;

        bool noError = true;
        const DatabaseName tenantDbName(boost::none, dbname);
        catalog::forEachCollectionFromDb(
            opCtx, tenantDbName, MODE_IS, [&](const CollectionPtr& collection) {
                auto collNss = collection->ns();

                if (collNss.size() - 1 <= dbname.size()) {
                    errmsg = str::stream()
                        << "weird fullCollectionName [" << collNss.toString() << "]";
                    noError = false;
                    return false;
                }

                if (repl::ReplicationCoordinator::isOplogDisabledForNS(collNss)) {
                    return true;
                }

                if (collNss.coll().startsWith("tmp.mr.")) {
                    // We skip any incremental map reduce collections as they also aren't
                    // replicated.
                    return true;
                }

                if (desiredCollections.size() > 0 &&
                    desiredCollections.count(collNss.coll().toString()) == 0)
                    return true;

                // Don't include 'drop pending' collections.
                if (collNss.isDropPendingNamespace())
                    return true;

                if (collection->isCapped()) {
                    cappedCollectionSet.insert(collNss.coll().toString());
                }

                collectionToUUIDMap.emplace(collNss.coll().toString(), collection->uuid());

                // Compute the hash for this collection.
                std::string hash = _hashCollection(opCtx, db, collNss);

                collectionToHashMap[collNss.coll().toString()] = hash;

                return true;
            });
        if (!noError)
            return false;

        BSONObjBuilder bb(result.subobjStart("collections"));
        BSONArrayBuilder cappedCollections;
        BSONObjBuilder collectionsByUUID;

        for (auto elem : cappedCollectionSet) {
            cappedCollections.append(elem);
        }

        for (auto entry : collectionToUUIDMap) {
            auto collName = entry.first;
            auto uuid = entry.second;
            uuid.appendToBuilder(&collectionsByUUID, collName);
        }

        for (auto entry : collectionToHashMap) {
            auto collName = entry.first;
            auto hash = entry.second;
            bb.append(collName, hash);
            md5_append(&globalState, (const md5_byte_t*)hash.c_str(), hash.size());
        }

        bb.done();

        result.append("capped", BSONArray(cappedCollections.done()));
        result.append("uuids", collectionsByUUID.done());

        md5digest d;
        md5_finish(&globalState, d);
        std::string hash = digestToString(d);

        result.append("md5", hash);
        result.appendNumber("timeMillis", timer.millis());

        return 1;
    }

private:
    std::string _hashCollection(OperationContext* opCtx, Database* db, const NamespaceString& nss) {

        CollectionPtr collection =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        invariant(collection);

        boost::optional<Lock::CollectionLock> collLock;
        if (opCtx->recoveryUnit()->getTimestampReadSource() ==
            RecoveryUnit::ReadSource::kProvided) {
            // When performing a read at a timestamp, we are only holding the database lock in
            // intent mode. We need to also acquire the collection lock in intent mode to ensure
            // reading from the consistent snapshot doesn't overlap with any catalog operations on
            // the collection.
            invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

            auto minSnapshot = collection->getMinimumVisibleSnapshot();
            auto mySnapshot = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
            invariant(mySnapshot);

            uassert(ErrorCodes::SnapshotUnavailable,
                    str::stream() << "Unable to read from a snapshot due to pending collection"
                                     " catalog changes; please retry the operation. Snapshot"
                                     " timestamp is "
                                  << mySnapshot->toString() << ". Collection minimum timestamp is "
                                  << minSnapshot->toString(),
                    !minSnapshot || *mySnapshot >= *minSnapshot);
        } else {
            invariant(opCtx->lockState()->isDbLockedForMode(db->name(), MODE_S));
        }

        auto desc = collection->getIndexCatalog()->findIdIndex(opCtx);

        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        if (desc) {
            exec = InternalPlanner::indexScan(opCtx,
                                              &collection,
                                              desc,
                                              BSONObj(),
                                              BSONObj(),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                              InternalPlanner::FORWARD,
                                              InternalPlanner::IXSCAN_FETCH);
        } else if (collection->isCapped() || collection->isClustered()) {
            exec = InternalPlanner::collectionScan(
                opCtx, &collection, PlanYieldPolicy::YieldPolicy::NO_YIELD);
        } else {
            LOGV2(20455, "Can't find _id index for namespace", "namespace"_attr = nss);
            return "no _id _index";
        }

        md5_state_t st;
        md5_init(&st);

        try {
            long long n = 0;
            BSONObj c;
            verify(nullptr != exec.get());
            while (exec->getNext(&c, nullptr) == PlanExecutor::ADVANCED) {
                md5_append(&st, (const md5_byte_t*)c.objdata(), c.objsize());
                n++;
            }
        } catch (DBException& exception) {
            LOGV2_WARNING(
                20456, "Error while hashing, db possibly dropped", "namespace"_attr = nss);
            exception.addContext("Plan executor error while running dbHash command");
            throw;
        }

        md5digest d;
        md5_finish(&st, d);
        std::string hash = digestToString(d);

        return hash;
    }

} dbhashCmd;

}  // namespace
}  // namespace mongo
