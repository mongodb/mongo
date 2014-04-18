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

#include "mongo/db/repl/ghost_sync.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/fail_point_service.h"

namespace mongo{

    extern Tee* rsLog;

    // For testing network failures in percolate() for chaining
    MONGO_FP_DECLARE(rsChaining1);
    MONGO_FP_DECLARE(rsChaining2);
    MONGO_FP_DECLARE(rsChaining3);

    GhostSync::~GhostSync() {
        log() << "~GhostSync() called" << rsLog;
    }

    void GhostSync::starting() {
        Client::initThread("rsGhostSync");
        replLocalAuth();
    }

    void GhostSync::clearCache() {
        rwlock lk(_lock, true);
        _ghostCache.clear();
    }

    void GhostSync::associateSlave(const BSONObj& id, const int memberId) {
        const OID rid = id["_id"].OID();
        rwlock lk( _lock , true );
        shared_ptr<GhostSlave> &g = _ghostCache[rid];
        if( g.get() == 0 ) {
            g.reset( new GhostSlave() );
            wassert( _ghostCache.size() < 10000 );
        }
        GhostSlave &slave = *g;
        if (slave.init) {
            LOG(1) << "tracking " << slave.slave->h().toString() << " as " << rid << rsLog;
            return;
        }

        slave.slave = (Member*)rs->findById(memberId);
        if (slave.slave != 0) {
            slave.init = true;
        }
        else {
            log() << "replset couldn't find a slave with id " << memberId
                  << ", not tracking " << rid << rsLog;
        }
    }

    bool GhostSync::updateSlave(const mongo::OID& rid, const OpTime& last) {
        rwlock lk( _lock , false );
        MAP::iterator i = _ghostCache.find( rid );
        if ( i == _ghostCache.end() ) {
            OCCASIONALLY warning() << "couldn't update position of the secondary with replSet _id '"
                                   << rid << "' because we have no entry for it" << rsLog;
            return false;
        }

        GhostSlave& slave = *(i->second);
        if (!slave.init) {
            OCCASIONALLY log() << "couldn't update position of the secondary with replSet _id '"
                               << rid << "' because it has not been initialized" << rsLog;
            return false;
        }

        ((ReplSetConfig::MemberCfg)slave.slave->config()).updateGroups(last);
        return true;
    }

    void GhostSync::percolate(const mongo::OID& rid, const OpTime& last) {
        shared_ptr<GhostSlave> slave;
        {
            rwlock lk( _lock , false );

            MAP::iterator i = _ghostCache.find( rid );
            if ( i == _ghostCache.end() ) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " no entry" << rsLog;
                return;
            }

            slave = i->second;
            if (!slave->init) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " not init" << rsLog;
                return;
            }
        }
        verify(slave->slave);

        // Keep trying to update until we either succeed or we become primary.
        // Note that this can block the ghostsync thread for quite a while if there
        // are connection problems to the current sync source ("sync target")
        while (true) {
            const Member *target = replset::BackgroundSync::get()->getSyncTarget();
            if (!target || rs->box.getState().primary()
                // we are currently syncing from someone who's syncing from us
                // the target might end up with a new Member, but s.slave never
                // changes so we'll compare the names
                || target == slave->slave || target->fullName() == slave->slave->fullName()) {
                LOG(1) << "replica set ghost target no good" << endl;
                return;
            }

            try {
                if (MONGO_FAIL_POINT(rsChaining1)) {
                    mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                        setMode(FailPoint::nTimes, 1);
                }

                // haveCursor() does not necessarily tell us if we have a non-dead cursor, 
                // so we check tailCheck() as well; see SERVER-8420
                slave->reader.tailCheck();
                if (!slave->reader.haveCursor()) {
                    if (!slave->reader.connect(rid, slave->slave->id(), target->fullName())) {
                        // error message logged in OplogReader::connect
                        sleepsecs(1);
                        continue;
                    }

                    if (MONGO_FAIL_POINT(rsChaining2)) {
                        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                            setMode(FailPoint::nTimes, 1);
                    }

                    slave->reader.ghostQueryGTE(rsoplog, last);
                    // if we lose the connection between connecting and querying, the cursor may not
                    // exist so we have to check again before using it.
                    if (!slave->reader.haveCursor()) {
                        sleepsecs(1);
                        continue;
                    }
                }

                LOG(5) << "replSet secondary " << slave->slave->fullName()
                       << " syncing progress updated from " << slave->last.toStringPretty()
                       << " to " << last.toStringPretty() << rsLog;
                if (slave->last > last) {
                    // Nothing to do; already up to date.
                    return;
                }

                while (slave->last <= last) {
                    if (MONGO_FAIL_POINT(rsChaining3)) {
                        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                            setMode(FailPoint::nTimes, 1);
                    }

                    if (!slave->reader.more()) {
                        // Hit the end of the oplog on the sync source; we're fully up to date now.
                        return;
                    }

                    BSONObj o = slave->reader.nextSafe();
                    slave->last = o["ts"]._opTime();
                }
                LOG(2) << "now last is " << slave->last.toString() << rsLog;
                // We moved the cursor forward enough; we're done.
                return;
            }
            catch (const DBException& e) {
                // This captures SocketExceptions as well.
                log() << "replSet ghost sync error: " << e.what() << " for "
                      << slave->slave->fullName() << rsLog;
                slave->reader.resetConnection();
            }
        }
    }

} // namespace mongo
