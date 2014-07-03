/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
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

#include "mongo/db/repl/repl_set_health_poll_task.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/heartbeat.h"
#include "mongo/db/repl/manager.h"
#include "mongo/db/repl/member.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

namespace repl {

    int ReplSetHealthPollTask::s_try_offset = 0;

    ReplSetHealthPollTask::ReplSetHealthPollTask(const HostAndPort& hh, const HeartbeatInfo& mm)
        : h(hh), m(mm), tries(s_try_offset), threshold(15),
          _timeout(ReplSetConfig::DEFAULT_HB_TIMEOUT) {

        if (theReplSet) {
            _timeout = theReplSet->config().getHeartbeatTimeout();
        }

        // doesn't need protection, all health tasks are created in a single thread
        s_try_offset += 7;
    }

    void ReplSetHealthPollTask::doWork() {
        if ( !theReplSet ) {
            LOG(2) << "replSet not initialized yet, skipping health poll this round" << rsLog;
            return;
        }

        HeartbeatInfo mem = m;
        HeartbeatInfo old = mem;
        try {
            BSONObj info;
            int theirConfigVersion = -10000;

            bool ok = _requestHeartbeat(mem, info, theirConfigVersion);

            // weight new ping with old pings
            // on the first ping, just use the ping value
            if (old.ping != 0) {
                mem.ping = (unsigned int)((old.ping * .8) + (mem.ping * .2));
            }

            if( ok ) {
                up(info, mem);
            }
            else if (info["code"].numberInt() == ErrorCodes::Unauthorized ||
                     info["errmsg"].str() == "unauthorized") {

                authIssue(mem);
            }
            else {
                down(mem, info.getStringField("errmsg"));
            }
        }
        catch (const DBException& e) {
            log() << "replSet health poll task caught a DBException: " << e.what();
            down(mem, e.what());
        }
        catch (const std::exception& e) {
            log() << "replSet health poll task caught an exception: " << e.what();
            down(mem, e.what());
        }
        m = mem;

        theReplSet->mgr->send( stdx::bind(&ReplSet::msgUpdateHBInfo, theReplSet, mem) );

        static time_t last = 0;
        time_t now = time(0);
        bool changed = mem.changed(old);
        if( changed ) {
            if( old.hbstate != mem.hbstate )
                log() << "replSet member " << h.toString() << " is now in state "
                      << mem.hbstate.toString() << rsLog;
        }
        if( changed || now-last>4 ) {
            last = now;
            theReplSet->mgr->send( stdx::bind(&Manager::msgCheckNewState, theReplSet->mgr) );
        }
    }

    bool ReplSetHealthPollTask::tryHeartbeat(BSONObj* info, int* theirConfigVersion) {
        bool ok = false;

        try {
            ok = requestHeartbeat(theReplSet->name(), theReplSet->selfFullName(),
                                  h.toString(), *info, theReplSet->config().version,
                                  *theirConfigVersion);
        }
        catch (DBException&) {
            // don't do anything, ok is already false
        }

        return ok;
    }

    bool ReplSetHealthPollTask::_requestHeartbeat(HeartbeatInfo& mem,
                                            BSONObj& info,
                                            int& theirConfigVersion) {
        {
            ScopedConn conn(h.toString());
            conn.setTimeout(_timeout);
            if (tries++ % threshold == (threshold - 1)) {
                conn.reconnect();
            }
        }

        Timer timer;
        time_t before = curTimeMicros64() / 1000000;

        bool ok = tryHeartbeat(&info, &theirConfigVersion);

        mem.ping = static_cast<unsigned int>(timer.millis());
        time_t totalSecs = mem.ping / 1000;

        // if that didn't work and we have more time, lower timeout and try again
        if (!ok && totalSecs < _timeout) {
            log() << "replset info " << h.toString() << " heartbeat failed, retrying" << rsLog;

            // lower timeout to remaining ping time
            {
                ScopedConn conn(h.toString());
                conn.setTimeout(_timeout - totalSecs);
            }

            int checkpoint = timer.millis();
            timer.reset();
            ok = tryHeartbeat(&info, &theirConfigVersion);
            mem.ping = static_cast<unsigned int>(timer.millis());
            totalSecs = (checkpoint + mem.ping)/1000;

            // set timeout back to default
            {
                ScopedConn conn(h.toString());
                conn.setTimeout(_timeout);
            }
        }

        // we set this on any response - we don't get this far if
        // couldn't connect because exception is thrown
        time_t after = mem.lastHeartbeat = before + totalSecs;

        if ( info["time"].isNumber() ) {
            long long t = info["time"].numberLong();
            if( t > after )
                mem.skew = (int) (t - after);
            else if( t < before )
                mem.skew = (int) (t - before); // negative
        }
        else {
            // it won't be there if remote hasn't initialized yet
            if( info.hasElement("time") )
                warning() << "heatbeat.time isn't a number: " << info << endl;
            mem.skew = INT_MIN;
        }

        {
            BSONElement state = info["state"];
            if( state.ok() )
                mem.hbstate = MemberState(state.Int());
        }

        if (info.hasField("stateDisagreement") && info["stateDisagreement"].trueValue()) {
            log() << "replset info " << h.toString() << " thinks that we are down" << endl;
        }

        return ok;
    }

    void ReplSetHealthPollTask::authIssue(HeartbeatInfo& mem) {
        mem.authIssue = true;
        mem.hbstate = MemberState::RS_UNKNOWN;

        // set health to 0 so that this doesn't count towards majority
        mem.health = 0.0;
        theReplSet->rmFromElectable(mem.id());
    }

    void ReplSetHealthPollTask::down(HeartbeatInfo& mem, string msg) {
        // if we've received a heartbeat from this member within the last two seconds, don't
        // change its state to down (if it's already down, leave it down since we don't have
        // any info about it other than it's heartbeating us)
        const Member* oldMemInfo = theReplSet->findById(mem.id());
        if (oldMemInfo && oldMemInfo->hbinfo().lastHeartbeatRecv+2 >= time(0)) {
            log() << "replset info " << h.toString()
                  << " just heartbeated us, but our heartbeat failed: " << msg
                  << ", not changing state" << rsLog;
            // we don't update any of the heartbeat info, though, since we didn't get any info
            // other than "not down" from having it heartbeat us
            return;
        }

        mem.authIssue = false;
        mem.health = 0.0;
        mem.ping = 0;
        if( mem.upSince || mem.downSince == 0 ) {
            mem.upSince = 0;
            mem.downSince = jsTime();
            mem.hbstate = MemberState::RS_DOWN;
            log() << "replSet info " << h.toString() << " is down (or slow to respond): "
                  << msg << rsLog;
        }
        mem.lastHeartbeatMsg = msg;
        theReplSet->rmFromElectable(mem.id());
    }

    void ReplSetHealthPollTask::up(const BSONObj& info, HeartbeatInfo& mem) {
        HeartbeatInfo::numPings++;
        mem.authIssue = false;

        if( mem.upSince == 0 ) {
            log() << "replSet member " << h.toString() << " is up" << rsLog;
            mem.upSince = mem.lastHeartbeat;
        }
        mem.health = 1.0;
        mem.lastHeartbeatMsg = info["hbmsg"].String();
        if (info.hasElement("syncingTo")) {
            mem.syncingTo = info["syncingTo"].String();
        }
        else {
            // empty out syncingTo since they are no longer syncing to anyone
            mem.syncingTo = "";
        }

        if( info.hasElement("opTime") )
            mem.opTime = info["opTime"].Date();

        // see if this member is in the electable set
        if( info["e"].eoo() ) {
            // for backwards compatibility
            const Member *member = theReplSet->findById(mem.id());
            if (member && member->config().potentiallyHot()) {
                theReplSet->addToElectable(mem.id());
            }
            else {
                theReplSet->rmFromElectable(mem.id());
            }
        }
        // add this server to the electable set if it is within 10
        // seconds of the latest optime we know of
        else if( info["e"].trueValue() &&
                 mem.opTime >= theReplSet->lastOpTimeWritten.getSecs() - 10) {
            unsigned lastOp = theReplSet->lastOtherOpTime().getSecs();
            if (lastOp > 0 && mem.opTime >= lastOp - 10) {
                theReplSet->addToElectable(mem.id());
            }
        }
        else {
            theReplSet->rmFromElectable(mem.id());
        }

        BSONElement cfg = info["config"];
        if( cfg.ok() ) {
            // received a new config
            stdx::function<void()> f =
                stdx::bind(&Manager::msgReceivedNewConfig, theReplSet->mgr, cfg.Obj().copy());
            theReplSet->mgr->send(f);
        }
        if (info.hasElement("electionTime")) {
            LOG(4) << "setting electionTime to " << info["electionTime"];
            mem.electionTime = info["electionTime"].Date();
        }
    }
} // namespace repl
} // namespace mongo
