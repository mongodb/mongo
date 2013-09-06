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

#include "mongo/db/repl/write_concern.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/util/background.h"
#include "mongo/util/mongoutils/str.h"

//#define REPLDEBUG(x) log() << "replBlock: "  << x << endl;
#define REPLDEBUG(x)

namespace mongo {

    // this is defined in fsync.cpp
    // need to figure out where to put for real
    bool lockedForWriting();

    using namespace mongoutils;

    class SlaveTracking : public BackgroundJob { // SERVER-4328 todo review
    public:
        string name() const { return "SlaveTracking"; }

        static const char * NS;

        struct Ident {

            Ident(const BSONObj& r, const BSONObj& config, const string& n) {
                BSONObjBuilder b;
                b.appendElements( r );
                b.append( "config" , config );
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
            cc().getAuthorizationSession()->grantInternalAuthorization();
            DBDirectClient db;
            while ( ! inShutdown() ) {
                sleepsecs( 1 );

                if ( ! _dirty )
                    continue;

                if ( inShutdown() )
                    return;
                
                if ( lockedForWriting() ) {
                    // note: there is still a race here
                    // since we could call fsyncLock between this and the last lock
                    RARELY log() << "can't update local.slaves because locked for writing" << endl;
                    continue;
                }

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

                _threadsWaitingForReplication.notify_all();
            }
        }

        void reset() {
            if ( _currentlyUpdatingCache )
                return;
            scoped_lock mylk(_mutex);
            _slaves.clear();
        }

        void update( const BSONObj& rid , const BSONObj config , const string& ns , OpTime last ) {
            REPLDEBUG( config << " " << rid << " " << ns << " " << last );

            Ident ident(rid, config, ns);

            scoped_lock mylk(_mutex);

            if (last > _slaves[ident]) {
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

                _threadsWaitingForReplication.notify_all();
            }
        }

        bool opReplicatedEnough( OpTime op , BSONElement w ) {
            RARELY {
                REPLDEBUG( "looking for : " << op << " w=" << w );
            }

            if (w.isNumber()) {
                return replicatedToNum(op, w.numberInt());
            }

            uassert( 16250 , "w has to be a string or a number" , w.type() == String );

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
            massert( 16805, "replicatedToNum called but not master anymore", _isMaster() );

            if ( w <= 1 )
                return true;

            w--; // now this is the # of slaves i need
            scoped_lock mylk(_mutex);
            return _replicatedToNum_slaves_locked( op, w );
        }

        bool waitForReplication(OpTime& op, int w, int maxSecondsToWait) {
            static const int noLongerMasterAssertCode = 16806;
            massert(noLongerMasterAssertCode, 
                    "waitForReplication called but not master anymore", _isMaster() );

            if ( w <= 1 )
                return true;

            w--; // now this is the # of slaves i need

            boost::xtime xt;
            boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
            xt.sec += maxSecondsToWait;
            
            scoped_lock mylk(_mutex);
            while ( ! _replicatedToNum_slaves_locked( op, w ) ) {
                if ( ! _threadsWaitingForReplication.timed_wait( mylk.boost() , xt ) ) {
                    massert(noLongerMasterAssertCode,
                            "waitForReplication called but not master anymore", _isMaster());
                    return false;
                }
                massert(noLongerMasterAssertCode, 
                        "waitForReplication called but not master anymore", _isMaster());
            }
            return true;
        }

        bool _replicatedToNum_slaves_locked(OpTime& op, int numSlaves ) {
            for ( map<Ident,OpTime>::iterator i=_slaves.begin(); i!=_slaves.end(); i++) {
                OpTime s = i->second;
                if ( s < op ) {
                    continue;
                }
                if ( --numSlaves == 0 )
                    return true;
            }
            return numSlaves <= 0;
        }

        std::vector<BSONObj> getHostsAtOp(OpTime& op) {
            std::vector<BSONObj> result;
            if (theReplSet) {
                result.push_back(theReplSet->myConfig().asBson());
            }

            scoped_lock mylk(_mutex);
            for (map<Ident,OpTime>::iterator i = _slaves.begin(); i != _slaves.end(); i++) {
                OpTime replicatedTo = i->second;
                if (replicatedTo >= op) {
                    result.push_back(i->first.obj["config"].Obj());
                }
            }

            return result;
        }

        unsigned getSlaveCount() const {
            scoped_lock mylk(_mutex);

            return _slaves.size();
        }

        // need to be careful not to deadlock with this
        mutable mongo::mutex _mutex;
        boost::condition _threadsWaitingForReplication;

        map<Ident,OpTime> _slaves;
        bool _dirty;
        bool _started;
        bool _currentlyUpdatingCache; // this is not thread safe, but ok for our purposes

    } slaveTracking;

    const char * SlaveTracking::NS = "local.slaves";

    // parse optimes from replUpdatePositionCommand and pass them to SyncSourceFeedback
    void updateSlaveLocations(BSONArray optimes) {
        BSONForEach(elem, optimes) {
            BSONObj entry = elem.Obj();
            BSONObj id = BSON("_id" << entry["_id"].OID());
            OpTime ot = entry["optime"]._opTime();
            BSONObj config = entry["config"].Obj();

            // update locally
            // This updates the slave tracking map, as well as updates
            // the GhostSlave cache, and updates the tag groups
            slaveTracking.update(id, config, "local.oplog.rs", ot);

            if (theReplSet && !theReplSet->isPrimary()) {
                // pass along if we are not primary
                LOG(2) << "percolating " << ot.toString() << " from " << entry << endl;
                theReplSet->syncSourceFeedback.percolate(entry["_id"].OID(), ot);
            }
        }
    }

    void updateSlaveLocation( CurOp& curop, const char * ns , OpTime lastOp ) {
        if ( lastOp.isNull() )
            return;

        verify( str::startsWith(ns, "local.oplog.") );

        Client * c = curop.getClient();
        verify(c);
        BSONObj rid = c->getRemoteID();
        if ( rid.isEmpty() )
            return;

        BSONObj handshake = c->getHandshake();
        if (handshake.hasField("config")) {
            slaveTracking.update(rid, handshake["config"].Obj(), ns, lastOp);
        }
        else {
            BSONObjBuilder bob;
            bob.append("host", curop.getRemoteString());
            bob.append("upgradeNeeded", true);
            slaveTracking.update(rid, bob.done(), ns, lastOp);
        }

        if (theReplSet && !theReplSet->isPrimary()) {
            // we don't know the slave's port, so we make the replica set keep
            // a map of rids to slaves
            // pass along if we are not primary
            LOG(2) << "getmore percolating " << lastOp.toString() << " from " << rid << endl;
            theReplSet->syncSourceFeedback.percolate(rid["_id"].OID(), lastOp);
        }
    }

    bool opReplicatedEnough( OpTime op , BSONElement w ) {
        return slaveTracking.opReplicatedEnough( op , w );
    }

    bool opReplicatedEnough( OpTime op , int w ) {
        return slaveTracking.replicatedToNum( op , w );
    }

    bool waitForReplication( OpTime op , int w , int maxSecondsToWait ) {
        return slaveTracking.waitForReplication( op, w, maxSecondsToWait );
    }

    vector<BSONObj> getHostsWrittenTo(OpTime& op) {
        return slaveTracking.getHostsAtOp(op);
    }

    void resetSlaveCache() {
        slaveTracking.reset();
    }

    unsigned getSlaveCount() {
        return slaveTracking.getSlaveCount();
    }
}
