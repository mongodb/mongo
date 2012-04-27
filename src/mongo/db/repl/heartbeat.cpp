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
*/

#include "pch.h"
#include "rs.h"
#include "health.h"
#include "../../util/background.h"

#include "../commands.h"
#include "../../util/concurrency/value.h"
#include "../../util/concurrency/task.h"
#include "../../util/concurrency/msg.h"
#include "../../util/mongoutils/html.h"
#include "../../util/goodies.h"
#include "../../util/ramlog.h"
#include "../helpers/dblogger.h"
#include "connections.h"
#include "../instance.h"
#include "../repl.h"

namespace mongo {

    using namespace bson;

    extern bool replSetBlind;
    extern ReplSettings replSettings;

    unsigned int HeartbeatInfo::numPings;

    long long HeartbeatInfo::timeDown() const {
        if( up() ) return 0;
        if( downSince == 0 )
            return 0; // still waiting on first heartbeat
        return jsTime() - downSince;
    }

    /* { replSetHeartbeat : <setname> } */
    class CmdReplSetHeartbeat : public ReplSetCommand {
    public:
        CmdReplSetHeartbeat() : ReplSetCommand("replSetHeartbeat") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( replSetBlind ) {
                if (theReplSet) {
                    errmsg = str::stream() << theReplSet->selfFullName() << " is blind";
                }
                return false;
            }

            /* we don't call ReplSetCommand::check() here because heartbeat
               checks many things that are pre-initialization. */
            if( !replSet ) {
                errmsg = "not running with --replSet";
                return false;
            }

            if (!checkAuth(errmsg, result)) {
                return false;
            }

            /* we want to keep heartbeat connections open when relinquishing primary.  tag them here. */
            {
                AbstractMessagingPort *mp = cc().port();
                if( mp )
                    mp->tag |= 1;
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
            int v = theReplSet->config().version;
            result.append("v", v);
            if( v > cmdObj["v"].Int() )
                result << "config" << theReplSet->config().asBson();

            return true;
        }
    } cmdReplSetHeartbeat;

    bool requestHeartbeat(string setName, string from, string memberFullName, BSONObj& result,
                          int myCfgVersion, int& theirCfgVersion, bool checkEmpty) {
        if( replSetBlind ) {
            return false;
        }

        BSONObj cmd = BSON( "replSetHeartbeat" << setName <<
                            "v" << myCfgVersion <<
                            "pv" << 1 <<
                            "checkEmpty" << checkEmpty <<
                            "from" << from );

        // generally not a great idea to do outbound waiting calls in a
        // write lock. heartbeats can be slow (multisecond to respond), so
        // generally we don't want to be locked, at least not without
        // thinking acarefully about it first.
        massert(15900, "can't heartbeat: too much lock",
                !Lock::somethingWriteLocked() || theReplSet == 0 || !theReplSet->lockedByMe() );

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
            : h(hh), m(mm), tries(s_try_offset), threshold(15) {
            // doesn't need protection, all health tasks are created in a single thread
            s_try_offset += 7;
        }

        string name() const { return "rsHealthPoll"; }

        void setUp() { Client::initThread( name().c_str() ); }

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
                else if (!info["errmsg"].eoo() &&
                         info["errmsg"].str() == "need to login") {
                    authIssue(mem);
                }
                else {
                    down(mem, info.getStringField("errmsg"));
                }
            }
            catch(DBException& e) {
                down(mem, e.what());
            }
            catch(...) {
                down(mem, "replSet unexpected exception in ReplSetHealthPollTask");
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
        bool _requestHeartbeat(HeartbeatInfo& mem, BSONObj& info, int& theirConfigVersion) {
            if (tries++ % threshold == (threshold - 1)) {
                ScopedConn conn(h.toString());
                conn.reconnect();
            }

            Timer timer;
            time_t before = curTimeMicros64() / 1000000;

            bool ok = requestHeartbeat(theReplSet->name(), theReplSet->selfFullName(),
                                       h.toString(), info, theReplSet->config().version, theirConfigVersion);

            mem.ping = (unsigned int)timer.millis();

            // we set this on any response - we don't get this far if
            // couldn't connect because exception is thrown
            time_t after = mem.lastHeartbeat = before + (mem.ping / 1000);

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

        boost::thread t(startSyncThread);

        task::fork(ghost);

        // member heartbeats are started in ReplSetImpl::initFromConfig
    }

}

/* todo:
   stop bg job and delete on removefromset
*/
