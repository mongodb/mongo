// dbhash.cpp

/**
*    Copyright (C) 2013-2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
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

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const override {
        return level == repl::ReadConcernLevel::kLocalReadConcern ||
            level == repl::ReadConcernLevel::kSnapshotReadConcern;
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

        // We lock the entire database in S-mode in order to ensure that the contents will not
        // change for the snapshot.
        auto lockMode = LockMode::MODE_S;
        if (repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
            // However, if we are using "atClusterTime" to read from a consistent snapshot, then we
            // only need to lock the database in intent mode to ensure that none of the collections
            // get dropped.
            lockMode = getLockModeForQuery(opCtx);
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

            if (desiredCollections.size() > 0 &&
                desiredCollections.count(collNss.coll().toString()) == 0)
                continue;

            // Don't include 'drop pending' collections.
            if (collNss.isDropPendingNamespace())
                continue;

            // Compute the hash for this collection.
            std::string hash = _hashCollection(opCtx, db, collNss.toString());

            bb.append(collNss.coll(), hash);
            md5_append(&globalState, (const md5_byte_t*)hash.c_str(), hash.size());
        }
        bb.done();

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
        if (repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
            // When using "atClusterTime", we are only holding the database lock in intent mode. We
            // need to also acquire the collection lock in intent mode to ensure reading from the
            // consistent snapshot doesn't overlap with any catalog operations on the collection.
            invariant(opCtx->lockState()->isDbLockedForMode(db->name(), MODE_IS));
            collLock.emplace(opCtx->lockState(), fullCollectionName, getLockModeForQuery(opCtx));

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

        IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex(opCtx);

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
