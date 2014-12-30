// ttl.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/ttl.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/background.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {

    Counter64 ttlPasses;
    Counter64 ttlDeletedDocuments;

    ServerStatusMetricField<Counter64> ttlPassesDisplay("ttl.passes", &ttlPasses);
    ServerStatusMetricField<Counter64> ttlDeletedDocumentsDisplay("ttl.deletedDocuments", &ttlDeletedDocuments);

    MONGO_EXPORT_SERVER_PARAMETER( ttlMonitorEnabled, bool, true );

    class TTLMonitor : public BackgroundJob {
    public:
        TTLMonitor(){}
        virtual ~TTLMonitor(){}

        virtual string name() const { return "TTLMonitor"; }

        static string secondsExpireField;

        virtual void run() {
            Client::initThread( name().c_str() );
            cc().getAuthorizationSession()->grantInternalAuthorization();

            while ( ! inShutdown() ) {
                sleepsecs( 60 );

                LOG(3) << "TTLMonitor thread awake" << endl;

                if ( !ttlMonitorEnabled ) {
                   LOG(1) << "TTLMonitor is disabled" << endl;
                   continue;
                }

                if ( lockedForWriting() ) {
                    // note: this is not perfect as you can go into fsync+lock between
                    // this and actually doing the delete later
                    LOG(3) << " locked for writing" << endl;
                    continue;
                }

                // Count it as active from the moment the TTL thread wakes up
                OperationContextImpl txn;

                // if part of replSet but not in a readable state (e.g. during initial sync), skip.
                if (repl::getGlobalReplicationCoordinator()->getReplicationMode() ==
                        repl::ReplicationCoordinator::modeReplSet &&
                        !repl::getGlobalReplicationCoordinator()->getCurrentMemberState().readable())
                    continue;

                set<string> dbs;
                dbHolder().getAllShortNames( dbs );

                ttlPasses.increment();

                for ( set<string>::const_iterator i=dbs.begin(); i!=dbs.end(); ++i ) {
                    string db = *i;

                    vector<BSONObj> indexes;
                    getTTLIndexesForDB(&txn, db, &indexes);

                    for ( vector<BSONObj>::const_iterator it = indexes.begin();
                          it != indexes.end(); ++it ) {

                        if ( !doTTLForIndex( &txn, db, *it ) ) {
                            break;  // stop processing TTL indexes on this database
                        }
                    }
                }
            }
        }

    private:
        /**
         * Acquire an IS-mode lock on the specified database and for each
         * collection in the database, append the specification of all
         * TTL indexes on those collections to the supplied vector.
         *
         * The index specifications are grouped by the collection to which
         * they belong.
         */
        void getTTLIndexesForDB( OperationContext* txn, const string& dbName,
                                 vector<BSONObj>* indexes ) {

            invariant( indexes && indexes->empty() );
            ScopedTransaction transaction( txn, MODE_IS );
            Lock::DBLock dbLock( txn->lockState(), dbName, MODE_IS );

            Database* db = dbHolder().get( txn, dbName );
            if ( !db ) {
                return;  // skip since database no longer exists
            }

            const DatabaseCatalogEntry* dbEntry = db->getDatabaseCatalogEntry();

            list<string> namespaces;
            dbEntry->getCollectionNamespaces( &namespaces );

            for ( list<string>::const_iterator it = namespaces.begin();
                  it != namespaces.end(); ++it ) {

                string ns = *it;
                Lock::CollectionLock collLock( txn->lockState(), ns, MODE_IS );
                CollectionCatalogEntry* coll = dbEntry->getCollectionCatalogEntry( txn, ns );

                if ( !coll ) {
                    continue;  // skip since collection not found in catalog
                }

                vector<string> indexNames;
                coll->getAllIndexes( txn, &indexNames );
                for ( size_t i = 0; i < indexNames.size(); i++ ) {
                    const string& name = indexNames[i];
                    BSONObj spec = coll->getIndexSpec( txn, name );

                    if ( spec.hasField( secondsExpireField ) ) {
                        indexes->push_back( spec.getOwned() );
                    }
                }
            }
        }

        /**
         * Remove documents from the collection using the specified TTL index
         * after a sufficient amount of time has passed according to its expiry
         * specification.
         *
         * @return true if caller should continue processing TTL indexes of collections
         *         on the specified database, and false otherwise
         */
        bool doTTLForIndex( OperationContext* txn, const string& dbName, const BSONObj& idx ) {
            BSONObj key = idx["key"].Obj();
            if ( key.nFields() != 1 ) {
                error() << "key for ttl index can only have 1 field" << endl;
                return true;
            }
            if ( !idx[secondsExpireField].isNumber() ) {
                log() << "ttl indexes require the " << secondsExpireField << " field to be "
                      << "numeric but received a type of: "
                      << typeName( idx[secondsExpireField].type() );
                return true;
            }

            BSONObj query;
            {
                BSONObjBuilder b;
                long long expireMs = 1000 * idx[secondsExpireField].numberLong();
                b.appendDate( "$lt", curTimeMillis64() - expireMs );
                query = BSON( key.firstElement().fieldName() << b.obj() );
            }

            LOG(1) << "TTL: " << key << " \t " << query << endl;

            long long numDeleted = 0;
            int attempt = 1;
            while (1) {
                const string ns = idx["ns"].String();

                ScopedTransaction scopedXact(txn, MODE_IX);
                AutoGetDb autoDb(txn, dbName, MODE_IX);
                Database* db = autoDb.getDb();
                if (!db) {
                    return false;
                }

                Lock::CollectionLock collLock( txn->lockState(), ns, MODE_IX );

                Collection* collection = db->getCollection( txn, ns );
                if ( !collection ) {
                    // collection was dropped
                    return true;
                }

                if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbName)) {
                    // we've stepped down since we started this function,
                    // so we should stop working as we only do deletes on the primary
                    return false;
                }

                if ( collection->getIndexCatalog()->findIndexByKeyPattern( txn, key ) == NULL ) {
                    // index not finished yet
                    LOG(1) << " skipping index because not finished";
                    return true;
                }

                try {
                    numDeleted = deleteObjects(txn,
                                               db,
                                               ns,
                                               query,
                                               PlanExecutor::YIELD_AUTO,
                                               false,
                                               true);
                    break;
                }
                catch (const WriteConflictException& dle) {
                    WriteConflictException::logAndBackoff(attempt++, "ttl", ns);
                }
            }

            ttlDeletedDocuments.increment(numDeleted);
            LOG(1) << "\tTTL deleted: " << numDeleted << endl;
            return true;
        }
    };

    void startTTLBackgroundJob() {
        TTLMonitor* ttl = new TTLMonitor();
        ttl->go();
    }

    string TTLMonitor::secondsExpireField = "expireAfterSeconds";
}
