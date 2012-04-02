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
#include "replutil.h"

//#define REPLDEBUG(x) log() << "replBlock: "  << x << endl;
#define REPLDEBUG(x)

namespace mongo {

    using namespace mongoutils;

    class SlaveTracking : public BackgroundJob { // SERVER-4328 todo review
    public:
        string name() const { return "SlaveTracking"; }

        static const char * NS;

        struct Ident {

            Ident(const BSONObj& r, const string& h, const string& n) {
                BSONObjBuilder b;
                b.appendElements( r );
                b.append( "host" , h );
                b.append( "ns" , n );
                obj = b.obj();
            }

            bool operator<( const Ident& other ) const {
                return obj["_id"].OID() < other.obj["_id"].OID();
            }

            BSONObj obj;
        };

        SlaveTracking() : _mutex("SlaveTracking") {
            _dirty = false;
            _started = false;
            _currentlyUpdatingCache = false;
        }

        void run() {
            Client::initThread( "slaveTracking" );
            DBDirectClient db;
            while ( ! inShutdown() ) {
                sleepsecs( 1 );

                if ( ! _dirty )
                    continue;

                if ( inShutdown() )
                    return;

                list< pair<BSONObj,BSONObj> > todo;

                {
                    scoped_lock mylk(_mutex);

                    for ( map<Ident,OpTime>::iterator i=_slaves.begin(); i!=_slaves.end(); i++ ) {
                        BSONObjBuilder temp;
                        temp.appendTimestamp( "syncedTo" , i->second.asDate() );
                        todo.push_back( pair<BSONObj,BSONObj>( i->first.obj.getOwned() ,
                                                               BSON( "$set" << temp.obj() ).getOwned() ) );
                    }
                    _dirty = false;
                }
                
                _currentlyUpdatingCache = true;
                for ( list< pair<BSONObj,BSONObj> >::iterator i=todo.begin(); i!=todo.end(); i++ ) {
                    db.update( NS , i->first , i->second , true );
                }
                _currentlyUpdatingCache = false;

            }
        }

        void reset() {
            if ( _currentlyUpdatingCache )
                return;
            scoped_lock mylk(_mutex);
            _slaves.clear();
        }

        void update( const BSONObj& rid , const string& host , const string& ns , OpTime last ) {
            REPLDEBUG( host << " " << rid << " " << ns << " " << last );

            Ident ident(rid,host,ns);

            scoped_lock mylk(_mutex);

            _slaves[ident] = last;
            _dirty = true;

            if (theReplSet && theReplSet->isPrimary()) {
                theReplSet->ghost->updateSlave(ident.obj["_id"].OID(), last);
            }

            if ( ! _started ) {
                // start background thread here since we definitely need it
                _started = true;
                go();
            }

        }

        bool opReplicatedEnough( OpTime op , BSONElement w ) {
            RARELY {
                REPLDEBUG( "looking for : " << op << " w=" << w );
            }

            if (w.isNumber()) {
                return replicatedToNum(op, w.numberInt());
            }

            if (!theReplSet) {
                return false;
            }

            string wStr = w.String();
            if (wStr == "majority") {
                // use the entire set, including arbiters, to prevent writing
                // to a majority of the set but not a majority of voters
                return replicatedToNum(op, theReplSet->config().getMajority());
            }

            map<string,ReplSetConfig::TagRule*>::const_iterator it = theReplSet->config().rules.find(wStr);
            uassert(14830, str::stream() << "unrecognized getLastError mode: " << wStr,
                    it != theReplSet->config().rules.end());

            return op <= (*it).second->last;
        }

        bool replicatedToNum(OpTime& op, int w) {
            if ( w <= 1 || ! _isMaster() )
                return true;

            w--; // now this is the # of slaves i need
            scoped_lock mylk(_mutex);
            for ( map<Ident,OpTime>::iterator i=_slaves.begin(); i!=_slaves.end(); i++) {
                OpTime s = i->second;
                if ( s < op ) {
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
        map<Ident,OpTime> _slaves;
        bool _dirty;
        bool _started;
        bool _currentlyUpdatingCache; // this is not thread safe, but ok for our purposes

    } slaveTracking;

    const char * SlaveTracking::NS = "local.slaves";

    void updateSlaveLocation( CurOp& curop, const char * ns , OpTime lastOp ) {
        if ( lastOp.isNull() )
            return;

        verify( str::startsWith(ns, "local.oplog.") );

        Client * c = curop.getClient();
        verify(c);
        BSONObj rid = c->getRemoteID();
        if ( rid.isEmpty() )
            return;

        slaveTracking.update( rid , curop.getRemoteString( false ) , ns , lastOp );

        if (theReplSet && !theReplSet->isPrimary()) {
            // we don't know the slave's port, so we make the replica set keep
            // a map of rids to slaves
            log(2) << "percolating " << lastOp.toString() << " from " << rid << endl;
            theReplSet->ghost->send( boost::bind(&GhostSync::percolate, theReplSet->ghost, rid, lastOp) );
        }
    }

    bool opReplicatedEnough( OpTime op , BSONElement w ) {
        return slaveTracking.opReplicatedEnough( op , w );
    }

    bool opReplicatedEnough( OpTime op , int w ) {
        return slaveTracking.replicatedToNum( op , w );
    }

    void resetSlaveCache() {
        slaveTracking.reset();
    }

    unsigned getSlaveCount() {
        return slaveTracking.getSlaveCount();
    }
}
