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
            if( !replSet ) {
                errmsg = "not a replset member";
                return false;
            }
            if( theReplSet == 0 ) { 
                errmsg = "still initializing";
                return false;
            }
            if( theReplSet->getName() != cmdObj.getStringField("replSetHeartbeat") ) { 
                errmsg = "set names do not match";
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

    private:
        void down() {
            m->_health = 0.0;
            if( m->_upSince ) {
                m->_upSince = 0;
                log() << "replSet " << m->fullName() << " is now down" << endl;
            }
        }

    public:

        void run() { 
            mongo::lastError.reset( new LastError() );
            DBClientConnection conn(true, 0, 10);
            conn._logLevel = 2;
            string err;
            conn.connect(m->fullName(), err);

            BSONObj cmd = BSON( "replSetHeartbeat" << theReplSet->getName() );
            while( 1 ) {
                try { 
                    BSONObj info;
                    bool ok = conn.runCommand("admin", cmd, info);
                    m->_lastHeartbeat = time(0);
                    if( ok ) {
                        if( m->_upSince == 0 ) {
                            log() << "replSet " << m->fullName() << " is now up" << endl;
                            m->_upSince = m->_lastHeartbeat;
                        }
                        m->_health = 1.0;
                        m->_lastHeartbeatErrMsg.set("");
                    }
                    else { 
                        down();
                        m->_lastHeartbeatErrMsg.set(info.getStringField("errmsg"));
                    }
                }
                catch(...) { 
                    down();
                    m->_lastHeartbeatErrMsg.set("connect/transport error");
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
            BSONObjBuilder bb;
            bb.append("name", m->fullName());
            bb.append("health", m->health());
            bb.append("uptime", (unsigned) (m->upSince() ? (time(0)-m->upSince()) : 0));
            bb.appendDate("lastHeartbeat", m->lastHeartbeat());
            bb.append("errmsg", m->_lastHeartbeatErrMsg.get());
            v.push_back(bb.obj());
            m = m->next();
        }
        b.append("set", getName());
        b.appendDate("date", time(0));
        b.append("members", v);
    }

    void ReplSet::startHealthThreads() {
        MemberInfo* m = _members.head();
        while( m ) {
            FeedbackThread *f = new FeedbackThread();
            f->m = m;
            f->go();
            m = m->next();
        }
    }

}

/* todo:
   stop bg job and delete on removefromset
*/
