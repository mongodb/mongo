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
#include "../../util/web/html.h"
#include "../../util/goodies.h"
#include "../helpers/dblogger.h"
#include "connections.h"

namespace mongo {
    /* decls for connections.h */
    ScopedConn::M& ScopedConn::_map = *(new ScopedConn::M());    
    mutex ScopedConn::mapMutex;
}

namespace mongo { 

    using namespace mongoutils::html;

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
            result.append("rs", true);
            if( theReplSet == 0 ) { 
                errmsg = "still initializing";
                return false;
            }
            if( theReplSet->getName() != cmdObj.getStringField("replSetHeartbeat") ) { 
                errmsg = "repl set names do not match";
                return false;
            }
            //result.append("set", theReplSet->getName());
            return true;
        }
    } cmdReplSetHeartbeat;

    /* poll every other set member to check its status */
    class FeedbackThread : public BackgroundJob {
    public:
        ReplSet::Member *m;

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

            BSONObj cmd = BSON( "replSetHeartbeat" << theReplSet->getName() );
            while( 1 ) {
                try { 
                    BSONObj info;
                    bool ok;
                    {
                        ScopedConn conn(m->fullName());
                        ok = conn->runCommand("admin", cmd, info);
                    }
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

    void ReplSet::Member::summarizeAsHtml(stringstream& s) const { 
        s << tr();
        {
            stringstream u;
            u << "http://" << _host << ':' << (_port + 1000) << "/_replSet";
            s << td( a(u.str(), "", fullName()) );
        }
        s << td(health());
        s << td(upSince());
        {
            stringstream h;
            time_t hb = lastHeartbeat();
            time_t now = time(0);
            if( hb == 0 ) h << "never"; 
            else {
                if( now > hb ) h << now-hb; 
                else h << 0;
                h << " secs ago";
            }
            s << td(h.str());
        }
        s << td(config().votes);
        s << td(_lastHeartbeatErrMsg.get());
        s << _tr();
    }

    string ReplSet::stateAsHtml(State s) { 
        if( s == STARTUP ) return a("", "serving still starting up, or still trying to initiate the set", "STARTUP");
        if( s == PRIMARY ) return a("", "this server thinks it is primary", "PRIMARY");
        if( s == SECONDARY ) return a("", "this server thinks it is a secondary (slave mode)", "SECONDARY");
        if( s == RECOVERING ) return a("", "recovering/resyncing; after recovery usually auto-transitions to secondary", "RECOVERING");
        if( s == FATAL ) return a("", "something bad has occurred and server is not completely offline with regard to the replica set.  fatal error.", "FATAL");
        return "???";
    }

    string ReplSet::stateAsStr(State s) { 
        if( s == STARTUP ) return "STARTUP";
        if( s == PRIMARY ) return "PRIMARY";
        if( s == SECONDARY ) return "SECONDARY";
        if( s == RECOVERING ) return "RECOVERING";
        if( s == FATAL ) return "FATAL";
        return "???";
    }

    void ReplSet::summarizeAsHtml(stringstream& s) const { 
        s << table(0, false);
        s << tr("Set name:", _name);
        s << tr("My state:", stateAsHtml(_myState));
        s << tr("Majority up:", elect.aMajoritySeemsToBeUp()?"yes":"no" );
        s << _table();

        const char *h[] = {"Member", "Up", "Uptime", 
            "<a title=\"when this server last received a heartbeat response - includes error code responses\">Last heartbeat</a>", 
            "Votes", "Status", 0};
        s << table(h);
        s << tr() << td(_self->fullName()) <<
            td("1") << 
            td("") << 
            td("") << 
            td(ToString(_self->config().votes)) << 
            td("self") << 
            _tr();
        Member *m = head();
        while( m ) {
            m->summarizeAsHtml(s);
            m = m->next();
        }
        s << _table();
    }

    void ReplSet::summarizeStatus(BSONObjBuilder& b) const { 
        Member *m =_members.head();
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
        b.append("myState", _myState);
        b.append("members", v);
    }

    void ReplSet::startHealthThreads() {
        Member* m = _members.head();
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
