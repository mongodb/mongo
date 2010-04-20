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

#include "stdafx.h"
#include "replset.h"
#include "health.h"
#include "../../util/background.h"
#include "../../client/dbclient.h"
#include "../commands.h"
#include "../../util/concurrency/value.h"

namespace mongo { 

    class CmdReplSetHeartbeat : public Command {
    public:
        virtual bool slaveOk() { return true; }
        virtual bool adminOnly() { return false; }
        virtual bool logTheOp() { return false; }
        virtual LockType locktype(){ return NONE; }
        CmdReplSetHeartbeat() : Command("replSetHeartbeat") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( theReplSet == 0 ) { 
                errmsg = "not a replset member";
                return false;
            }
            result.append("set", theReplSet->getName());
            return true;
        }
    } cmdReplSetHeartbeat;

    /* poll every other set member to check its status */
    class FeedbackThread : public BackgroundJob {
    public:
        ReplSet::MemberInfo *m;
        void run() { 
            DBClientConnection conn(true, 0, 10);
            while( 1 ) {
                try { 
                    BSONObj info;
                    bool ok = conn.simpleCommand("admin", &info, "replSetHeartbeat");
                    log() << "TEMP heartbeat " << ok << ' ' << info.toString() << endl;
                    if( ok ) {
                        m->lastHeartbeat = time(0);
                    }
                }
                catch(...) { 
                    log() << "TEMP heartbeat not ok" << endl;
                }
                sleepsecs(2);
            }
        }
    };

    void ReplSet::summarizeStatus(BSONObjBuilder& b) const { 
        MemberInfo *m =_members.head();
        vector<BSONObj> v;

        // add self
        {
            HostAndPort h(getHostName(), cmdLine.port);
            v.push_back( BSON( "name" << h.toString() << "self" << true ) );
        }

        while( m ) {
            v.push_back( BSON( "name" << m->fullName() ) );
            m = m->next();
        }
        b.append("members", v);
    }

    void ReplSet::startHealthThreads() {
        MemberInfo* m = _members.head();
        while( m ) {
            FeedbackThread *f = new FeedbackThread();
            f->m = m;
            m = m->next();
        }
    }

}

/* todo:
   stop bg job and delete on removefromset
*/
