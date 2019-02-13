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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <map>
#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/timer.h"

namespace mongo {

namespace {

class DBHashCmd : public ErrmsgCommandDeprecated {
public:
    DBHashCmd() : ErrmsgCommandDeprecated("dbHash", "dbhash") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
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

            auto* replCoord = repl::ReplicationCoordinator::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalReadAtClusterTime' option is only supported when replication is"
                    " enabled",
                    replCoord->isReplEnabled());

            auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalReadAtClusterTime' option is only supported by storage engines"
                    " that support document-level concurrency",
                    storageEngine->supportsDocLocking());

            uassert(ErrorCodes::TypeMismatch,
                    "The '$_internalReadAtClusterTime' option must be a Timestamp",
                    elem.type() == BSONType::bsonTimestamp);

            auto targetClusterTime = elem.timestamp();

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
                                  << "; last applied opTime: "
                                  << lastAppliedOpTime.toString(),
                    lastAppliedOpTime.getTimestamp() >= targetClusterTime);

            // We aren't holding the global lock in intent mode, so it is possible for the global
            // storage engine to have been destructed already as a result of the server shutting
            // down. This isn't an actual concern because the testing infrastructure won't use the
            // $_internalReadAtClusterTime option in any test suite where clean shutdown is expected
            // to occur concurrently with tests running.
            auto allCommittedTime = storageEngine->getAllCommittedTimestamp();
            invariant(!allCommittedTime.isNull());

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "$_internalReadAtClusterTime value must not be greater"
                                     " than the all-committed timestamp. Requested clusterTime: "
                                  << targetClusterTime.toString()
                                  << "; all-committed timestamp: "
                                  << allCommittedTime.toString(),
                    allCommittedTime >= targetClusterTime);

            // The $_internalReadAtClusterTime option causes any storage-layer cursors created
            // during plan execution to read from a consistent snapshot of data at the supplied
            // clusterTime, even across yields.
            opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                          targetClusterTime);

            // The $_internalReadAtClusterTime option also causes any storage-layer cursors created
            // during plan execution to block on prepared transactions.
            opCtx->recoveryUnit()->setIgnorePrepared(false);
        }

        // We lock the entire database in S-mode in order to ensure that the contents will not
        // change for the snapshot.
        auto lockMode = LockMode::MODE_S;
        if (opCtx->recoveryUnit()->getTimestampReadSource() ==
            RecoveryUnit::ReadSource::kProvided) {
            // However, if we are performing a read at a timestamp, then we only need to lock the
            // database in intent mode to ensure that none of the collections get dropped.
            lockMode = LockMode::MODE_IS;
        }
        AutoGetDb autoDb(opCtx, ns, lockMode);
        Database* db = autoDb.getDb();
        std::list<std::string> colls;
        if (db) {
            db->getDatabaseCatalogEntry()->getCollectionNamespaces(&colls);
            colls.sort();
        }

        result.append("host", prettyHostName());

        md5_state_t globalState;
        md5_init(&globalState);

        // A set of 'system' collections that are replicated, and therefore included in the db hash.
        const std::set<StringData> replicatedSystemCollections{"system.backup_users",
                                                               "system.js",
                                                               "system.new_users",
                                                               "system.roles",
                                                               "system.users",
                                                               "system.version",
                                                               "system.views"};

        BSONArrayBuilder cappedCollections;
        BSONObjBuilder collectionsByUUID;

        BSONObjBuilder bb(result.subobjStart("collections"));
        for (const auto& collectionName : colls) {

            NamespaceString collNss(collectionName);

            if (collNss.size() - 1 <= dbname.size()) {
                errmsg = str::stream() << "weird fullCollectionName [" << collNss.toString() << "]";
                return false;
            }

            // Only include 'system' collections that are replicated.
            bool isReplicatedSystemColl =
                (replicatedSystemCollections.count(collNss.coll().toString()) > 0);
            if (collNss.isSystem() && !isReplicatedSystemColl)
                continue;

            if (collNss.coll().startsWith("tmp.mr.")) {
                // We skip any incremental map reduce collections as they also aren't replicated.
                continue;
            }

            if (desiredCollections.size() > 0 &&
                desiredCollections.count(collNss.coll().toString()) == 0)
                continue;

            // Don't include 'drop pending' collections.
            if (collNss.isDropPendingNamespace())
                continue;

            if (Collection* collection = db->getCollection(opCtx, collectionName)) {
                if (collection->isCapped()) {
                    cappedCollections.append(collNss.coll());
                }

                if (OptionalCollectionUUID uuid = collection->uuid()) {
                    uuid->appendToBuilder(&collectionsByUUID, collNss.coll());
                }
            }

            // Compute the hash for this collection.
            std::string hash = _hashCollection(opCtx, db, collNss.toString());

            bb.append(collNss.coll(), hash);
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
    std::string _hashCollection(OperationContext* opCtx,
                                Database* db,
                                const std::string& fullCollectionName) {

        NamespaceString ns(fullCollectionName);

        Collection* collection = db->getCollection(opCtx, ns);
        if (!collection)
            return "";

        boost::optional<Lock::CollectionLock> collLock;
        if (opCtx->recoveryUnit()->getTimestampReadSource() ==
            RecoveryUnit::ReadSource::kProvided) {
            // When performing a read at a timestamp, we are only holding the database lock in
            // intent mode. We need to also acquire the collection lock in intent mode to ensure
            // reading from the consistent snapshot doesn't overlap with any catalog operations on
            // the collection.
            invariant(opCtx->lockState()->isDbLockedForMode(db->name(), MODE_IS));
            collLock.emplace(opCtx->lockState(), fullCollectionName, MODE_IS);

            auto minSnapshot = collection->getMinimumVisibleSnapshot();
            auto mySnapshot = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
            invariant(mySnapshot);

            uassert(ErrorCodes::SnapshotUnavailable,
                    str::stream() << "Unable to read from a snapshot due to pending collection"
                                     " catalog changes; please retry the operation. Snapshot"
                                     " timestamp is "
                                  << mySnapshot->toString()
                                  << ". Collection minimum timestamp is "
                                  << minSnapshot->toString(),
                    !minSnapshot || *mySnapshot >= *minSnapshot);
        } else {
            invariant(opCtx->lockState()->isDbLockedForMode(db->name(), MODE_S));
        }

        auto desc = collection->getIndexCatalog()->findIdIndex(opCtx);

        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        if (desc) {
            exec = InternalPlanner::indexScan(opCtx,
                                              collection,
                                              desc,
                                              BSONObj(),
                                              BSONObj(),
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanExecutor::NO_YIELD,
                                              InternalPlanner::FORWARD,
                                              InternalPlanner::IXSCAN_FETCH);
        } else if (collection->isCapped()) {
            exec = InternalPlanner::collectionScan(
                opCtx, fullCollectionName, collection, PlanExecutor::NO_YIELD);
        } else {
            log() << "can't find _id index for: " << fullCollectionName;
            return "no _id _index";
        }

        md5_state_t st;
        md5_init(&st);

        long long n = 0;
        PlanExecutor::ExecState state;
        BSONObj c;
        verify(NULL != exec.get());
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&c, NULL))) {
            md5_append(&st, (const md5_byte_t*)c.objdata(), c.objsize());
            n++;
        }
        if (PlanExecutor::IS_EOF != state) {
            warning() << "error while hashing, db dropped? ns=" << fullCollectionName;
            uasserted(34371,
                      "Plan executor error while running dbHash command: " +
                          WorkingSetCommon::toStatusString(c));
        }
        md5digest d;
        md5_finish(&st, d);
        std::string hash = digestToString(d);

        return hash;
    }

} dbhashCmd;

}  // namespace
}  // namespace mongo
