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
*/

#include "pch.h"

#include "mongo/db/ttl.h"
#include "mongo/db/databaseholder.h"
#include "mongo/db/instance.h"
#include "mongo/db/ops/delete.h"
#include "mongo/util/background.h"
#include "mongo/db/replutil.h"

namespace mongo {
    
    // this is defined in fsync.cpp
    // need to figure out where to put for real
    bool lockedForWriting();
    

    class TTLMonitor : public BackgroundJob {
    public:
        TTLMonitor(){}
        virtual ~TTLMonitor(){}

        virtual string name() const { return "TTLMonitor"; }
        
        static string secondsExpireField;
        
        void doTTLForDB( const string& dbName ) {
            
            if ( ! isMasterNs( dbName.c_str() ) )
                return;
            
            Client::GodScope god;

            vector<BSONObj> indexes;
            {
                auto_ptr<DBClientCursor> cursor = db.query( dbName + ".system.indexes" , 
                                                            BSON( secondsExpireField << BSON( "$exists" << true ) ) );
                if ( cursor.get() ) {
                    while ( cursor->more() ) {
                        indexes.push_back( cursor->next().getOwned() );
                    }
                }
            }
            
            for ( unsigned i=0; i<indexes.size(); i++ ) {
                BSONObj idx = indexes[i];
                

                BSONObj key = idx["key"].Obj();
                uassert( 16230 , "key for ttl index can only have 1 field" , key.nFields() == 1 );

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
                    NamespaceDetails* nsd = nsdetails( ns.c_str() );
                    if ( nsd->setUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) ) {
                        nsd->syncUserFlags( ns );
                    }
                    n = deleteObjects( ns.c_str() , query , false , true );
                }

                LOG(1) << "\tTTL deleted: " << n << endl;
            }
            
            
        }

        virtual void run() {
            Client::initThread( name().c_str() );

            while ( ! inShutdown() ) {
                sleepsecs( 60 );
                
                LOG(3) << "TTLMonitor thread awake" << endl;
                
                if ( lockedForWriting() ) {
                    // note: this is not perfect as you can go into fsync+lock between 
                    // this and actually doing the delete later
                    LOG(3) << " locked for writing" << endl;
                    continue;
                }

                set<string> dbs;
                {
                    Lock::DBRead lk( "local" );
                    dbHolder().getAllShortNames( dbs );
                }
                
                for ( set<string>::const_iterator i=dbs.begin(); i!=dbs.end(); ++i ) {
                    string db = *i;
                    doTTLForDB( db );
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
