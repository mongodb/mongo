// database_holder.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/background.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kStorage);

    static DatabaseHolder _dbHolder;

    DatabaseHolder& dbHolder() {
        return _dbHolder;
    }

    Database* DatabaseHolder::get(OperationContext* txn,
                                  const StringData& ns) const {

        txn->lockState()->assertAtLeastReadLocked(ns);

        SimpleMutex::scoped_lock lk(_m);
        const StringData db = _todb( ns );
        DBs::const_iterator it = _dbs.find(db);
        if ( it != _dbs.end() )
            return it->second;
        return NULL;
    }

    Database* DatabaseHolder::getOrCreate(OperationContext* txn,
                                          const StringData& ns,
                                          bool& justCreated) {

        const StringData dbname = _todb( ns );
        invariant(txn->lockState()->isAtLeastReadLocked(dbname));

        if (txn->lockState()->isWriteLocked() && FileAllocator::get()->hasFailed()) {
            uassert(17507, "Can't take a write lock while out of disk space", false);
        }

        {
            SimpleMutex::scoped_lock lk(_m);
            {
                DBs::const_iterator i = _dbs.find(dbname);
                if( i != _dbs.end() ) {
                    justCreated = false;
                    return i->second;
                }
            }

            // todo: protect against getting sprayed with requests for different db names that DNE -
            //       that would make the DBs map very large.  not clear what to do to handle though,
            //       perhaps just log it, which is what we do here with the "> 40" :
            bool cant = !txn->lockState()->isWriteLocked(ns);
            if( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1)) ||
                _dbs.size() > 40 || cant || DEBUG_BUILD ) {
                log() << "opening db: " << dbname;
            }
            massert(15927, "can't open database in a read lock. if db was just closed, consider retrying the query. might otherwise indicate an internal error", !cant);
        }

        // we know we have a db exclusive lock here
        { // check casing
            string duplicate = Database::duplicateUncasedName(dbname.toString());
            if ( !duplicate.empty() ) {
                stringstream ss;
                ss << "db already exists with different case already have: [" << duplicate
                   << "] trying to create [" << dbname.toString() << "]";
                uasserted( DatabaseDifferCaseCode , ss.str() );
            }
        }

        // we mark our thread as having done writes now as we do not want any exceptions
        // once we start creating a new database
        cc().writeHappened();

        // this locks _m for defensive checks, so we don't want to be locked right here :
        StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
        invariant(storageEngine);
        DatabaseCatalogEntry* entry = storageEngine->getDatabaseCatalogEntry( txn, dbname );
        invariant( entry );
        justCreated = !entry->exists();
        Database *db = new Database(txn,
                                    dbname,
                                    entry );

        {
            SimpleMutex::scoped_lock lk(_m);
            _dbs[dbname] = db;
        }

        return db;
    }

    void DatabaseHolder::close(OperationContext* txn,
                               const StringData& ns) {
        invariant(txn->lockState()->isW());

        StringData db = _todb(ns);

        SimpleMutex::scoped_lock lk(_m);
        DBs::const_iterator it = _dbs.find(db);
        if ( it == _dbs.end() )
            return;

        it->second->close( txn );
        delete it->second;
        _dbs.erase( db );

        getGlobalEnvironment()->getGlobalStorageEngine()->closeDatabase( txn, db.toString() );
    }

    bool DatabaseHolder::closeAll(OperationContext* txn,
                                  BSONObjBuilder& result,
                                  bool force) {
        invariant(txn->lockState()->isW());

        getDur().commitNow(txn); // bad things happen if we close a DB with outstanding writes

        SimpleMutex::scoped_lock lk(_m);

        set< string > dbs;
        for ( DBs::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i ) {
            dbs.insert( i->first );
        }

        BSONArrayBuilder bb( result.subarrayStart( "dbs" ) );
        int nNotClosed = 0;
        for( set< string >::iterator i = dbs.begin(); i != dbs.end(); ++i ) {
            string name = *i;

            LOG(2) << "DatabaseHolder::closeAll name:" << name;

            if( !force && BackgroundOperation::inProgForDb(name) ) {
                log() << "WARNING: can't close database "
                      << name
                      << " because a bg job is in progress - try killOp command"
                      << endl;
                nNotClosed++;
                continue;
            }

            Database* db = _dbs[name];
            db->close( txn );
            delete db;

            _dbs.erase( name );

            getGlobalEnvironment()->getGlobalStorageEngine()->closeDatabase( txn, name );

            bb.append( name );
        }

        bb.done();
        if( nNotClosed ) {
            result.append("nNotClosed", nNotClosed);
        }

        return true;
    }
}
