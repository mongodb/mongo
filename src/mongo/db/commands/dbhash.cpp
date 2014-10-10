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

#include "mongo/db/commands/dbhash.h"

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/timer.h"

namespace mongo {

    DBHashCmd dbhashCmd;


    void logOpForDbHash(const char* ns) {
        dbhashCmd.wipeCacheForCollection( ns );
    }

    // ----

    DBHashCmd::DBHashCmd()
        : Command( "dbHash", false, "dbhash" ),
          _cachedHashedMutex( "_cachedHashedMutex" ){
    }

    void DBHashCmd::addRequiredPrivileges(const std::string& dbname,
                                          const BSONObj& cmdObj,
                                          std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dbHash);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    string DBHashCmd::hashCollection( OperationContext* opCtx, Database* db, const string& fullCollectionName, bool* fromCache ) {
        scoped_ptr<scoped_lock> cachedHashedLock;

        if ( isCachable( fullCollectionName ) ) {
            cachedHashedLock.reset( new scoped_lock( _cachedHashedMutex ) );
            string hash = _cachedHashed[fullCollectionName];
            if ( hash.size() > 0 ) {
                *fromCache = true;
                return hash;
            }
        }

        *fromCache = false;
        Collection* collection = db->getCollection( opCtx, fullCollectionName );
        if ( !collection )
            return "";

        IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex( opCtx );

        auto_ptr<PlanExecutor> exec;
        if ( desc ) {
            exec.reset(InternalPlanner::indexScan(opCtx,
                                                  collection,
                                                  desc,
                                                  BSONObj(),
                                                  BSONObj(),
                                                  false,
                                                  InternalPlanner::FORWARD,
                                                  InternalPlanner::IXSCAN_FETCH));
        }
        else if ( collection->isCapped() ) {
            exec.reset(InternalPlanner::collectionScan(opCtx,
                                                       fullCollectionName,
                                                       collection));
        }
        else {
            log() << "can't find _id index for: " << fullCollectionName << endl;
            return "no _id _index";
        }

        md5_state_t st;
        md5_init(&st);

        long long n = 0;
        PlanExecutor::ExecState state;
        BSONObj c;
        verify(NULL != exec.get());
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&c, NULL))) {
            md5_append( &st , (const md5_byte_t*)c.objdata() , c.objsize() );
            n++;
        }
        if (PlanExecutor::IS_EOF != state) {
            warning() << "error while hashing, db dropped? ns=" << fullCollectionName << endl;
        }
        md5digest d;
        md5_finish(&st, d);
        string hash = digestToString( d );

        if ( cachedHashedLock.get() ) {
            _cachedHashed[fullCollectionName] = hash;
        }

        return hash;
    }

    bool DBHashCmd::run(OperationContext* txn, const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
        Timer timer;

        set<string> desiredCollections;
        if ( cmdObj["collections"].type() == Array ) {
            BSONObjIterator i( cmdObj["collections"].Obj() );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( e.type() != String ) {
                    errmsg = "collections entries have to be strings";
                    return false;
                }
                desiredCollections.insert( e.String() );
            }
        }

        list<string> colls;
        const string ns = parseNs(dbname, cmdObj);

        // We lock the entire database in S-mode in order to ensure that the contents will not
        // change for the snapshot.
        AutoGetDb autoDb(txn, ns, MODE_S);
        Database* db = autoDb.getDb();
        if (db) {
            db->getDatabaseCatalogEntry()->getCollectionNamespaces(&colls);
            colls.sort();
        }

        result.appendNumber( "numCollections" , (long long)colls.size() );
        result.append( "host" , prettyHostName() );

        md5_state_t globalState;
        md5_init(&globalState);

        vector<string> cached;

        BSONObjBuilder bb( result.subobjStart( "collections" ) );
        for ( list<string>::iterator i=colls.begin(); i != colls.end(); i++ ) {
            string fullCollectionName = *i;
            if ( fullCollectionName.size() -1 <= dbname.size() ) {
                errmsg  = str::stream() << "weird fullCollectionName [" << fullCollectionName << "]";
                return false;
            }
            string shortCollectionName = fullCollectionName.substr( dbname.size() + 1 );

            if ( shortCollectionName.find( "system." ) == 0 )
                continue;

            if ( desiredCollections.size() > 0 &&
                 desiredCollections.count( shortCollectionName ) == 0 )
                continue;

            bool fromCache = false;
            string hash = hashCollection( txn, db, fullCollectionName, &fromCache );

            bb.append( shortCollectionName, hash );

            md5_append( &globalState , (const md5_byte_t*)hash.c_str() , hash.size() );
            if ( fromCache )
                cached.push_back( fullCollectionName );
        }
        bb.done();

        md5digest d;
        md5_finish(&globalState, d);
        string hash = digestToString( d );

        result.append( "md5" , hash );
        result.appendNumber( "timeMillis", timer.millis() );

        result.append( "fromCache", cached );

        return 1;
    }

    void DBHashCmd::wipeCacheForCollection( const StringData& ns ) {
        if ( !isCachable( ns ) )
            return;
        scoped_lock lk( _cachedHashedMutex );
        _cachedHashed.erase( ns.toString() );
    }

    bool DBHashCmd::isCachable( const StringData& ns ) const {
        return ns.startsWith( "config." );
    }

}
