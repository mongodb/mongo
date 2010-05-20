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
#include "replset.h"
#include "health.h"
#include "../../util/background.h"
#include "../../client/dbclient.h"
#include "../commands.h"
#include "../../util/concurrency/value.h"
#include "../../util/concurrency/task.h"
#include "../../util/concurrency/msg.h"
#include "../../util/mongoutils/html.h"
#include "../../util/goodies.h"
#include "../../util/ramlog.h"
#include "../helpers/dblogger.h"
#include "connections.h"
#include "../../util/unittest.h"


namespace mongo { 

    using namespace bson;

    /* { replSetHeartbeat : <setname> } */
    class CmdReplSetHeartbeat : public Command {
    public:
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual bool logTheOp() { return false; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const { help<<"internal"; }
        CmdReplSetHeartbeat() : Command("replSetHeartbeat") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !replSet ) {
                errmsg = "not a replset member";
                return false;
            }
            if( cmdObj["pv"].Int() != 1 ) { 
                errmsg = "incompatible replset protocol version";
                return false;
            }
            string s = string(cmdObj.getStringField("replSetHeartbeat"))+'/';
            if( !startsWith(cmdLine.replSet, s ) ) {
                errmsg = "repl set names do not match";
                cout << "cmdline: " << cmdLine.replSet << endl;
                cout << "s: " << s << endl;
                result.append("mismatch", true);
                return false;
            }
            result.append("rs", true);
            if( theReplSet == 0 ) { 
                errmsg = "still initializing";
                return false;
            }

            if( theReplSet->name() != cmdObj.getStringField("replSetHeartbeat") ) { 
                errmsg = "repl set names do not match (2)";
                result.append("mismatch", true);
                return false;
            }
            result.append("set", theReplSet->name());
            result.append("state", theReplSet->state());
            int v = theReplSet->config().version;
            result.append("v", v);
            if( v > cmdObj["v"].Int() )
                result << "config" << theReplSet->config().asBson();
            return true;
        }
    } cmdReplSetHeartbeat;

    /* throws dbexception */
    bool requestHeartbeat(string setName, string memberFullName, BSONObj& result, int myCfgVersion, int& theirCfgVersion) { 
        BSONObj cmd = BSON( "replSetHeartbeat" << setName << "v" << myCfgVersion << "pv" << 1 );
        ScopedConn conn(memberFullName);
        return conn->runCommand("admin", cmd, result);
    }

    /* poll every other set member to check its status */
    class ReplSetHealthPoll : public task::Task {
        HostAndPort h;
        HeartbeatInfo m;
    public:
        ReplSetHealthPoll(const HostAndPort& hh, const HeartbeatInfo& mm) : h(hh), m(mm)
        { }

        string name() { return "ReplSetHealthPoll"; }
        void doWork() { 
            HeartbeatInfo mem = m;
            HeartbeatInfo old = mem;
            try { 
                BSONObj info;
                int theirConfigVersion = -10000;
                bool ok = requestHeartbeat(theReplSet->name(), h.toString(), info, theReplSet->config().version, theirConfigVersion);
                mem.lastHeartbeat = time(0); // we set this on any response - we don't get this far if couldn't connect because exception is thrown
                {
                    be state = info["state"];
                    if( state.ok() )
                        mem.hbstate = (MemberState) state.Int();
                }
                if( ok ) {
                    if( mem.upSince == 0 ) {
                        log() << "replSet info " << h.toString() << " is now up" << rsLog;
                        mem.upSince = mem.lastHeartbeat;
                    }
                    mem.health = 1.0;
                    mem.lastHeartbeatMsg = "";

                    be cfg = info["config"];
                    if( cfg.ok() ) {
                        // received a new config
                        theReplSet->mgr->send(cfg.Obj().copy());
                    }
                }
                else { 
                    down(mem, info.getStringField("errmsg"));
                }
            }
            catch(...) { 
                down(mem, "connect/transport error");             
            }
            m = mem;
            if( mem.changed(old) ) {
                theReplSet->mgr->send(ReplSet::Manager::CheckNewState);
            }
        }

    private:
        void down(HeartbeatInfo& mem, string msg) {
            mem.health = 0.0;
            if( mem.upSince ) {
                mem.upSince = 0;
                log() << "replSet info " << h.toString() << " is now down" << rsLog;
            }
            mem.lastHeartbeatMsg = msg;
        }
    };
    
    /** called during repl set startup.  caller expects it to return fairly quickly. 
        note ReplSet object is only created once we get a config - so this won't run 
        until the initiation.
    */
    void ReplSet::startThreads() {
        task::fork(mgr->taskPtr());

        Member* m = _members.head();
        while( m ) {
            ReplSetHealthPoll *task = new ReplSetHealthPoll(m->h(), m->hbinfo());
            task::repeat(shared_ptr<task::Task>(task), 2000);
            m = m->next();
        }

        mgr->send(Manager::CheckNewState);
    }

}

/* todo:
   stop bg job and delete on removefromset
*/
