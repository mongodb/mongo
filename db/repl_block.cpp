// repl_block.cpp

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
#include "repl.h"
#include "repl_block.h"
#include "instance.h"
#include "dbhelpers.h"
#include "../util/background.h"
#include "../util/mongoutils/str.h"
#include "../client/dbclient.h"
#include "replpair.h"

//#define REPLDEBUG(x) log() << "replBlock: "  << x << endl;
#define REPLDEBUG(x)

namespace mongo {

    using namespace mongoutils;

    class SlaveTracking : public BackgroundJob {
    public:
        string name() const { return "SlaveTracking"; }

        static const char * NS;

        struct Ident {
            
            Ident(BSONObj r,string h,string n){
                BSONObjBuilder b;
                b.appendElements( r );
                b.append( "host" , h );
                b.append( "ns" , n );
                obj = b.obj();
            }

            bool operator<( const Ident& other ) const {
                return obj.woCompare( other.obj ) < 0;
            }
            
            BSONObj obj;
        };

        struct Info {
            Info() : loc(0){}
            ~Info(){
                if ( loc && owned ){
                    delete loc;
                }
            }
            bool owned;
            OpTime * loc;
        };

        SlaveTracking() : _mutex("SlaveTracking") {
            _dirty = false;
            _started = false;
        }

        void run(){
            Client::initThread( "slaveTracking" );
            DBDirectClient db;
            while ( ! inShutdown() ){
                sleepsecs( 1 );

                if ( ! _dirty )
                    continue;
                
                writelock lk(NS);

                list< pair<BSONObj,BSONObj> > todo;
                
                {
                    scoped_lock mylk(_mutex);
                    
                    for ( map<Ident,Info>::iterator i=_slaves.begin(); i!=_slaves.end(); i++ ){
                        BSONObjBuilder temp;
                        temp.appendTimestamp( "syncedTo" , i->second.loc[0].asDate() );
                        todo.push_back( pair<BSONObj,BSONObj>( i->first.obj.getOwned() , 
                                                               BSON( "$set" << temp.obj() ).getOwned() ) );
                    }
                    
                    _slaves.clear();
                }

                for ( list< pair<BSONObj,BSONObj> >::iterator i=todo.begin(); i!=todo.end(); i++ ){
                    db.update( NS , i->first , i->second , true );
                }

                _dirty = false;
            }
        }

        void reset(){
            scoped_lock mylk(_mutex);
            _slaves.clear();
        }

        void update( const BSONObj& rid , const string& host , const string& ns , OpTime last ){
            REPLDEBUG( host << " " << rid << " " << ns << " " << last );

            scoped_lock mylk(_mutex);
            
#ifdef _DEBUG
            MongoFileAllowWrites allowWrites;
#endif

            Ident ident(rid,host,ns);
            Info& i = _slaves[ ident ];
            if ( i.loc ){
                i.loc[0] = last;
                return;
            }
            
            dbMutex.assertAtLeastReadLocked();

            BSONObj res;
            if ( Helpers::findOne( NS , ident.obj , res ) ){
                assert( res["syncedTo"].type() );
                i.owned = false;
                i.loc = (OpTime*)res["syncedTo"].value();
                i.loc[0] = last;
                return;
            }
            
            i.owned = true;
            i.loc = new OpTime[1];
            i.loc[0] = last;
            _dirty = true;

            if ( ! _started ){
                // start background thread here since we definitely need it
                _started = true;
                go();
            }

        }
        
        bool opReplicatedEnough( OpTime op , int w ){
            RARELY {
                REPLDEBUG( "looking for : " << op << " w=" << w );
            }

            if ( w <= 1 || ! _isMaster() )
                return true;

            w--; // now this is the # of slaves i need
            scoped_lock mylk(_mutex);
            for ( map<Ident,Info>::iterator i=_slaves.begin(); i!=_slaves.end(); i++){
                OpTime s = *(i->second.loc);
                if ( s < op ){
                    continue;
                }
                if ( --w == 0 )
                    return true;
            }
            return w <= 0;
        }

        unsigned getSlaveCount() const {
            scoped_lock mylk(_mutex);

            return _slaves.size();
        }
        
        // need to be careful not to deadlock with this
        mutable mongo::mutex _mutex;
        map<Ident,Info> _slaves;
        bool _dirty;
        bool _started;

    } slaveTracking;

    const char * SlaveTracking::NS = "local.slaves";

    void updateSlaveLocation( CurOp& curop, const char * ns , OpTime lastOp ){
        if ( lastOp.isNull() )
            return;
        
        assert( str::startsWith(ns, "local.oplog.") );
        
        Client * c = curop.getClient();
        assert(c);
        BSONObj rid = c->getRemoteID();
        if ( rid.isEmpty() )
            return;

        slaveTracking.update( rid , curop.getRemoteString( false ) , ns , lastOp );
    }

    bool opReplicatedEnough( OpTime op , int w ){
        return slaveTracking.opReplicatedEnough( op , w );
    }

    void resetSlaveCache(){
        slaveTracking.reset();
    }

    unsigned getSlaveCount(){
        return slaveTracking.getSlaveCount();
    }
}
