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

#include "mongo/pch.h"

#include "mongo/db/ttl.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/database_holder.h"
#include "mongo/db/instance.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/background.h"

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
        
        void doTTLForDB( const string& dbName ) {

            bool isMaster = isMasterNs( dbName.c_str() );
            vector<BSONObj> indexes;
            {
                auto_ptr<DBClientCursor> cursor =
                                db.query( dbName + ".system.indexes" ,
                                          BSON( secondsExpireField << BSON( "$exists" << true ) ) ,
                                          0 , /* default nToReturn */
                                          0 , /* default nToSkip */
                                          0 , /* default fieldsToReturn */
                                          QueryOption_SlaveOk ); /* perform on secondaries too */
                if ( cursor.get() ) {
                    while ( cursor->more() ) {
                        indexes.push_back( cursor->next().getOwned() );
                    }
                }
            }
            
            for ( unsigned i=0; i<indexes.size(); i++ ) {
                BSONObj idx = indexes[i];
                

                BSONObj key = idx["key"].Obj();
                if ( key.nFields() != 1 ) {
                    error() << "key for ttl index can only have 1 field" << endl;
                    continue;
                }

                BSONObj query;
                {
                    BSONObjBuilder b;
                    b.appendDate( "$lt" , curTimeMillis64() - ( 1000 * idx[secondsExpireField].numberLong() ) );
                    query = BSON( key.firstElement().fieldName() << b.obj() );
                }
                
                LOG(1) << "TTL: " << key << " \t " << query << endl;
                
                long long n = 0;
                {
                    string ns = idx["ns"].String();
                    Client::WriteContext ctx( ns );
                    NamespaceDetails* nsd = nsdetails( ns );
                    if ( ! nsd ) {
                        // collection was dropped
                        continue;
                    }
                    if ( nsd->setUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) ) {
                        nsd->syncUserFlags( ns );
                    }
                    // only do deletes if on master
                    if ( ! isMaster ) {
                        continue;
                    }

                    n = deleteObjects( ns.c_str() , query , false , true );
                    ttlDeletedDocuments.increment( n );
                }

                LOG(1) << "\tTTL deleted: " << n << endl;
            }
            
            
        }

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

                // if part of replSet but not in a readable state (e.g. during initial sync), skip.
                if ( theReplSet && !theReplSet->state().readable() )
                    continue;

                set<string> dbs;
                {
                    Lock::DBRead lk( "local" );
                    dbHolder().getAllShortNames( dbs );
                }
                
                ttlPasses.increment();

                for ( set<string>::const_iterator i=dbs.begin(); i!=dbs.end(); ++i ) {
                    string db = *i;
                    try {
                        doTTLForDB( db );
                    }
                    catch ( DBException& e ) {
                        error() << "error processing ttl for db: " << db << " " << e << endl;
                    }
                }

            }
        }

        DBDirectClient db;
    };

    void startTTLBackgroundJob() {
        TTLMonitor* ttl = new TTLMonitor();
        ttl->go();
    }    
    
    string TTLMonitor::secondsExpireField = "expireAfterSeconds";
}
