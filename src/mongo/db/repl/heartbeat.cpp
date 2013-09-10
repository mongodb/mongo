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

#include "mongo/pch.h"

#include <boost/thread/thread.hpp>

#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/health.h"
#include "mongo/db/repl/replication_server_status.h"  // replSettings
#include "mongo/db/repl/rs.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/msg.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/goodies.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/ramlog.h"

namespace mongo {

    using namespace bson;

    MONGO_FP_DECLARE(rsDelayHeartbeatResponse);
    MONGO_FP_DECLARE(rsStopHeartbeatRequest);

    extern bool replSetBlind;

    unsigned int HeartbeatInfo::numPings;

    long long HeartbeatInfo::timeDown() const {
        if( up() ) return 0;
        if( downSince == 0 )
            return 0; // still waiting on first heartbeat
        return jsTime() - downSince;
    }

    void HeartbeatInfo::updateFromLastPoll(const HeartbeatInfo& newInfo) {
        hbstate = newInfo.hbstate;
        health = newInfo.health;
        upSince = newInfo.upSince;
        downSince = newInfo.downSince;
        lastHeartbeat = newInfo.lastHeartbeat;
        lastHeartbeatMsg = newInfo.lastHeartbeatMsg;
        // Note: lastHeartbeatRecv is updated through CmdReplSetHeartbeat::run().

        syncingTo = newInfo.syncingTo;
        opTime = newInfo.opTime;
        skew = newInfo.skew;
        authIssue = newInfo.authIssue;
        ping = newInfo.ping;
    }

    /* { replSetHeartbeat : <setname> } */
    class CmdReplSetHeartbeat : public ReplSetCommand {
    public:
        CmdReplSetHeartbeat() : ReplSetCommand("replSetHeartbeat") { }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetHeartbeat);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( replSetBlind ) {
                if (theReplSet) {
                    errmsg = str::stream() << theReplSet->selfFullName() << " is blind";
                }
                return false;
            }

            MONGO_FAIL_POINT_BLOCK(rsDelayHeartbeatResponse, delay) {
                const BSONObj& data = delay.getData();
                sleepsecs(data["delay"].numberInt());
            }

            /* we don't call ReplSetCommand::check() here because heartbeat
               checks many things that are pre-initialization. */
            if( !replSet ) {
                errmsg = "not running with --replSet";
                return false;
            }

            /* we want to keep heartbeat connections open when relinquishing primary.  tag them here. */
            {
                AbstractMessagingPort *mp = cc().port();
                if( mp )
                    mp->tag |= ScopedConn::keepOpen;
            }

            if( cmdObj["pv"].Int() != 1 ) {
                errmsg = "incompatible replset protocol version";
                return false;
            }
            {
                string s = string(cmdObj.getStringField("replSetHeartbeat"));
                if( cmdLine.ourSetName() != s ) {
                    errmsg = "repl set names do not match";
                    log() << "replSet set names do not match, our cmdline: " << cmdLine._replSet << rsLog;
                    log() << "replSet s: " << s << rsLog;
                    result.append("mismatch", true);
                    return false;
                }
            }

            result.append("rs", true);
            if( cmdObj["checkEmpty"].trueValue() ) {
                result.append("hasData", replHasDatabases());
            }
            if( (theReplSet == 0) || (theReplSet->startupStatus == ReplSetImpl::LOADINGCONFIG) ) {
                string from( cmdObj.getStringField("from") );
                if( !from.empty() ) {
                    scoped_lock lck( replSettings.discoveredSeeds_mx );
                    replSettings.discoveredSeeds.insert(from);
                }
                result.append("hbmsg", "still initializing");
                return true;
            }

            if( theReplSet->name() != cmdObj.getStringField("replSetHeartbeat") ) {
                errmsg = "repl set names do not match (2)";
                result.append("mismatch", true);
                return false;
            }
            result.append("set", theReplSet->name());
            result.append("state", theReplSet->state().s);
            result.append("e", theReplSet->iAmElectable());
            result.append("hbmsg", theReplSet->hbmsg());
            result.append("time", (long long) time(0));
            result.appendDate("opTime", theReplSet->lastOpTimeWritten.asDate());
            const Member *syncTarget = replset::BackgroundSync::get()->getSyncTarget();
            if (syncTarget) {
                result.append("syncingTo", syncTarget->fullName());
            }

            int v = theReplSet->config().version;
            result.append("v", v);
            if( v > cmdObj["v"].Int() )
                result << "config" << theReplSet->config().asBson();

            Member *from = theReplSet->findByName(cmdObj.getStringField("from"));
            if (!from) {
                return true;
            }

            // if we thought that this node is down, let it know
            if (!from->hbinfo().up()) {
                result.append("stateDisagreement", true);
            }

            // note that we got a heartbeat from this node
            theReplSet->mgr->send(boost::bind(&ReplSet::msgUpdateHBRecv,
                                              theReplSet, from->hbinfo().id(), time(0)));


            return true;
        }
    } cmdReplSetHeartbeat;

    bool requestHeartbeat(const std::string& setName,
                          const std::string& from,
                          const std::string& memberFullName,
                          BSONObj& result,
                          int myCfgVersion,
                          int& theirCfgVersion,
                          bool checkEmpty) {
        if( replSetBlind ) {
            return false;
        }

        MONGO_FAIL_POINT_BLOCK(rsStopHeartbeatRequest, member) {
            const BSONObj& data = member.getData();
            const std::string& stopMember = data["member"].str();

            if (memberFullName == stopMember) {
                return false;
            }
        }

        BSONObj cmd = BSON( "replSetHeartbeat" << setName <<
                            "v" << myCfgVersion <<
                            "pv" << 1 <<
                            "checkEmpty" << checkEmpty <<
                            "from" << from );

        ScopedConn conn(memberFullName);
        return conn.runCommand("admin", cmd, result, 0);
    }

    /**
     * Poll every other set member to check its status.
     *
     * A detail about local machines and authentication: suppose we have 2
     * members, A and B, on the same machine using different keyFiles. A is
     * primary. If we're just starting the set, there are no admin users, so A
     * and B can access each other because it's local access.
     *
     * Then we add a user to A. B cannot sync this user from A, because as soon
     * as we add a an admin user, A requires auth. However, A can still
     * heartbeat B, because B *doesn't* have an admin user.  So A can reach B
     * but B cannot reach A.
     *
     * Once B is restarted with the correct keyFile, everything should work as
     * expected.
     */
    class ReplSetHealthPollTask : public task::Task {
    private:
        /**
         * Each healthpoll task reconnects periodically.  By starting each task at a different
         * number for "tries", the tasks will reconnect at different times, minimizing the impact
         * of network blips.
         */
        static int s_try_offset;

        HostAndPort h;
        HeartbeatInfo m;
        int tries;
        const int threshold;
    public:
        ReplSetHealthPollTask(const HostAndPort& hh, const HeartbeatInfo& mm)
            : h(hh), m(mm), tries(s_try_offset), threshold(15),
              _timeout(ReplSetConfig::DEFAULT_HB_TIMEOUT) {

            if (theReplSet) {
                _timeout = theReplSet->config().getHeartbeatTimeout();
            }

            // doesn't need protection, all health tasks are created in a single thread
            s_try_offset += 7;
        }

        string name() const { return "rsHealthPoll"; }

        void setUp() { }

        void doWork() {
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

            theReplSet->mgr->send( boost::bind(&ReplSet::msgUpdateHBInfo, theReplSet, mem) );

            static time_t last = 0;
            time_t now = time(0);
            bool changed = mem.changed(old);
            if( changed ) {
                if( old.hbstate != mem.hbstate )
                    log() << "replSet member " << h.toString() << " is now in state " << mem.hbstate.toString() << rsLog;
            }
            if( changed || now-last>4 ) {
                last = now;
                theReplSet->mgr->send( boost::bind(&Manager::msgCheckNewState, theReplSet->mgr) );
            }
        }

    private:
        bool tryHeartbeat(BSONObj* info, int* theirConfigVersion) {
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

        bool _requestHeartbeat(HeartbeatInfo& mem, BSONObj& info, int& theirConfigVersion) {
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
                be state = info["state"];
                if( state.ok() )
                    mem.hbstate = MemberState(state.Int());
            }

            if (info.hasField("stateDisagreement") && info["stateDisagreement"].trueValue()) {
                log() << "replset info " << h.toString() << " thinks that we are down" << endl;
            }

            return ok;
        }

        void authIssue(HeartbeatInfo& mem) {
            mem.authIssue = true;
            mem.hbstate = MemberState::RS_UNKNOWN;

            // set health to 0 so that this doesn't count towards majority
            mem.health = 0.0;
            theReplSet->rmFromElectable(mem.id());
        }

        void down(HeartbeatInfo& mem, string msg) {
            // if we've received a heartbeat from this member within the last two seconds, don't
            // change its state to down (if it's already down, leave it down since we don't have
            // any info about it other than it's heartbeating us)
            if (m.lastHeartbeatRecv+2 >= time(0)) {
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
                log() << "replSet info " << h.toString() << " is down (or slow to respond): " << msg << rsLog;
            }
            mem.lastHeartbeatMsg = msg;
            theReplSet->rmFromElectable(mem.id());
        }

        void up(const BSONObj& info, HeartbeatInfo& mem) {
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

            be cfg = info["config"];
            if( cfg.ok() ) {
                // received a new config
                boost::function<void()> f =
                    boost::bind(&Manager::msgReceivedNewConfig, theReplSet->mgr, cfg.Obj().copy());
                theReplSet->mgr->send(f);
            }
        }

        // Heartbeat timeout
        time_t _timeout;
    };

    int ReplSetHealthPollTask::s_try_offset = 0;

    void ReplSetImpl::endOldHealthTasks() {
        unsigned sz = healthTasks.size();
        for( set<ReplSetHealthPollTask*>::iterator i = healthTasks.begin(); i != healthTasks.end(); i++ )
            (*i)->halt();
        healthTasks.clear();
        if( sz )
            DEV log() << "replSet debug: cleared old tasks " << sz << endl;
    }

    void ReplSetImpl::startHealthTaskFor(Member *m) {
        DEV log() << "starting rsHealthPoll for " << m->fullName() << endl;
        ReplSetHealthPollTask *task = new ReplSetHealthPollTask(m->h(), m->hbinfo());
        healthTasks.insert(task);
        task::repeat(task, 2000);
    }

    void startSyncThread();

    /** called during repl set startup.  caller expects it to return fairly quickly.
        note ReplSet object is only created once we get a config - so this won't run
        until the initiation.
    */
    void ReplSetImpl::startThreads() {
        task::fork(mgr);
        mgr->send( boost::bind(&Manager::msgCheckNewState, theReplSet->mgr) );

        if (myConfig().arbiterOnly) {
            return;
        }

        boost::thread t(startSyncThread);

        replset::BackgroundSync* sync = replset::BackgroundSync::get();
        boost::thread producer(boost::bind(&replset::BackgroundSync::producerThread, sync));
        boost::thread notifier(boost::bind(&replset::BackgroundSync::notifierThread, sync));

        task::fork(ghost);

        // member heartbeats are started in ReplSetImpl::initFromConfig
    }

}

/* todo:
   stop bg job and delete on removefromset
*/
