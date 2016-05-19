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

#include "mongo/db/commands/dbhash.h"

#include <map>
#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/timer.h"

namespace mongo {

using std::list;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {

class DBHashCmd : public Command {
public:
    DBHashCmd() : Command("dbHash", false, "dbhash") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool slaveOk() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dbHash);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Timer timer;

        set<string> desiredCollections;
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

        list<string> colls;
        const string ns = parseNs(dbname, cmdObj);

        // We lock the entire database in S-mode in order to ensure that the contents will not
        // change for the snapshot.
        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetDb autoDb(txn, ns, MODE_S);
        Database* db = autoDb.getDb();
        if (db) {
            db->getDatabaseCatalogEntry()->getCollectionNamespaces(&colls);
            colls.sort();
        }

        result.append("host", prettyHostName());

        md5_state_t globalState;
        md5_init(&globalState);

        vector<string> cached;

        const std::initializer_list<StringData> replicatedSystemCollections{"system.backup_users",
                                                                            "system.js",
                                                                            "system.new_users",
                                                                            "system.roles",
                                                                            "system.users",
                                                                            "system.version"};


        BSONObjBuilder bb(result.subobjStart("collections"));
        for (list<string>::iterator i = colls.begin(); i != colls.end(); i++) {
            string fullCollectionName = *i;
            if (fullCollectionName.size() - 1 <= dbname.size()) {
                errmsg = str::stream() << "weird fullCollectionName [" << fullCollectionName << "]";
                return false;
            }
            string shortCollectionName = fullCollectionName.substr(dbname.size() + 1);

            if (shortCollectionName.find("system.") == 0 &&
                std::find(replicatedSystemCollections.begin(),
                          replicatedSystemCollections.end(),
                          shortCollectionName) == replicatedSystemCollections.end())
                continue;

            if (desiredCollections.size() > 0 && desiredCollections.count(shortCollectionName) == 0)
                continue;

            bool fromCache = false;
            string hash = _hashCollection(txn, db, fullCollectionName, &fromCache);

            bb.append(shortCollectionName, hash);

            md5_append(&globalState, (const md5_byte_t*)hash.c_str(), hash.size());
            if (fromCache)
                cached.push_back(fullCollectionName);
        }
        bb.done();

        md5digest d;
        md5_finish(&globalState, d);
        string hash = digestToString(d);

        result.append("md5", hash);
        result.appendNumber("timeMillis", timer.millis());

        result.append("fromCache", cached);

        return 1;
    }

    void wipeCacheForCollection(OperationContext* txn, const NamespaceString& ns) {
        if (!_isCachable(ns))
            return;

        txn->recoveryUnit()->onCommit([this, txn, ns] {
            stdx::lock_guard<stdx::mutex> lk(_cachedHashedMutex);
            if (ns.isCommand()) {
                // The <dbName>.$cmd namespace can represent a command that
                // modifies the entire database, e.g. dropDatabase, so we remove
                // the cached entries for all collections in the database.
                _cachedHashed.erase(ns.db().toString());
            } else {
                _cachedHashed[ns.db().toString()].erase(ns.coll().toString());
            }
        });
    }

private:
    bool _isCachable(const NamespaceString& ns) const {
        return ns.isConfigDB();
    }

    std::string _hashCollection(OperationContext* opCtx,
                                Database* db,
                                const std::string& fullCollectionName,
                                bool* fromCache) {
        stdx::unique_lock<stdx::mutex> cachedHashedLock(_cachedHashedMutex, stdx::defer_lock);

        NamespaceString ns(fullCollectionName);

        if (_isCachable(ns)) {
            cachedHashedLock.lock();
            string hash = _cachedHashed[ns.db().toString()][ns.coll().toString()];
            if (hash.size() > 0) {
                *fromCache = true;
                return hash;
            }
        }

        *fromCache = false;
        Collection* collection = db->getCollection(fullCollectionName);
        if (!collection)
            return "";

        IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex(opCtx);

        unique_ptr<PlanExecutor> exec;
        if (desc) {
            exec = InternalPlanner::indexScan(opCtx,
                                              collection,
                                              desc,
                                              BSONObj(),
                                              BSONObj(),
                                              false,  // endKeyInclusive
                                              PlanExecutor::YIELD_MANUAL,
                                              InternalPlanner::FORWARD,
                                              InternalPlanner::IXSCAN_FETCH);
        } else if (collection->isCapped()) {
            exec = InternalPlanner::collectionScan(
                opCtx, fullCollectionName, collection, PlanExecutor::YIELD_MANUAL);
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
        string hash = digestToString(d);

        if (cachedHashedLock.owns_lock()) {
            _cachedHashed[ns.db().toString()][ns.coll().toString()] = hash;
        }

        return hash;
    }

    stdx::mutex _cachedHashedMutex;
    std::map<std::string, std::map<std::string, std::string>> _cachedHashed;

} dbhashCmd;

}  // namespace

void logOpForDbHash(OperationContext* txn, const char* ns) {
    NamespaceString nsString(ns);
    dbhashCmd.wipeCacheForCollection(txn, nsString);
}

}  // namespace mongo
