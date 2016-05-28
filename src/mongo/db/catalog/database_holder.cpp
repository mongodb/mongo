/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"

namespace mongo {

using std::set;
using std::string;
using std::stringstream;

namespace {

StringData _todb(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        uassert(13074, "db name can't be empty", ns.size());
        return ns;
    }

    uassert(13075, "db name can't be empty", i > 0);

    const StringData d = ns.substr(0, i);
    uassert(13280,
            "invalid db name: " + ns.toString(),
            NamespaceString::validDBName(d, NamespaceString::DollarInDbNameBehavior::Allow));

    return d;
}


DatabaseHolder _dbHolder;

}  // namespace


DatabaseHolder& dbHolder() {
    return _dbHolder;
}


Database* DatabaseHolder::get(OperationContext* txn, StringData ns) const {
    const StringData db = _todb(ns);
    invariant(txn->lockState()->isDbLockedForMode(db, MODE_IS));

    stdx::lock_guard<SimpleMutex> lk(_m);
    DBs::const_iterator it = _dbs.find(db);
    if (it != _dbs.end()) {
        return it->second;
    }

    return NULL;
}

Database* DatabaseHolder::openDb(OperationContext* txn, StringData ns, bool* justCreated) {
    const StringData dbname = _todb(ns);
    invariant(txn->lockState()->isDbLockedForMode(dbname, MODE_X));

    Database* db = get(txn, ns);
    if (db) {
        if (justCreated) {
            *justCreated = false;
        }

        return db;
    }

    // Check casing
    const string duplicate = Database::duplicateUncasedName(dbname.toString());
    if (!duplicate.empty()) {
        stringstream ss;
        ss << "db already exists with different case already have: [" << duplicate
           << "] trying to create [" << dbname.toString() << "]";
        uasserted(ErrorCodes::DatabaseDifferCase, ss.str());
    }

    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    invariant(storageEngine);

    DatabaseCatalogEntry* entry = storageEngine->getDatabaseCatalogEntry(txn, dbname);
    invariant(entry);
    const bool exists = entry->exists();
    if (!exists) {
        audit::logCreateDatabase(&cc(), dbname);
    }

    if (justCreated) {
        *justCreated = !exists;
    }

    // Do this outside of the scoped lock, because database creation does transactional
    // operations which may block. Only one thread can be inside this method for the same DB
    // name, because of the requirement for X-lock on the database when we enter. So there is
    // no way we can insert two different databases for the same name.
    db = new Database(txn, dbname, entry);

    stdx::lock_guard<SimpleMutex> lk(_m);
    _dbs[dbname] = db;

    return db;
}

void DatabaseHolder::close(OperationContext* txn, StringData ns) {
    // TODO: This should be fine if only a DB X-lock
    invariant(txn->lockState()->isW());

    const StringData dbName = _todb(ns);

    stdx::lock_guard<SimpleMutex> lk(_m);

    DBs::const_iterator it = _dbs.find(dbName);
    if (it == _dbs.end()) {
        return;
    }

    it->second->close(txn);
    delete it->second;
    _dbs.erase(it);

    getGlobalServiceContext()->getGlobalStorageEngine()->closeDatabase(txn, dbName.toString());
}

bool DatabaseHolder::closeAll(OperationContext* txn, BSONObjBuilder& result, bool force) {
    invariant(txn->lockState()->isW());

    stdx::lock_guard<SimpleMutex> lk(_m);

    set<string> dbs;
    for (DBs::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
        dbs.insert(i->first);
    }

    BSONArrayBuilder bb(result.subarrayStart("dbs"));
    int nNotClosed = 0;
    for (set<string>::iterator i = dbs.begin(); i != dbs.end(); ++i) {
        string name = *i;

        LOG(2) << "DatabaseHolder::closeAll name:" << name;

        if (!force && BackgroundOperation::inProgForDb(name)) {
            log() << "WARNING: can't close database " << name
                  << " because a bg job is in progress - try killOp command";
            nNotClosed++;
            continue;
        }

        Database* db = _dbs[name];
        db->close(txn);
        delete db;

        _dbs.erase(name);

        getGlobalServiceContext()->getGlobalStorageEngine()->closeDatabase(txn, name);

        bb.append(name);
    }

    bb.done();
    if (nNotClosed) {
        result.append("nNotClosed", nNotClosed);
    }

    return true;
}
}
