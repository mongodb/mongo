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
#include "../../util/mongoutils/html.h"
#include "../../util/goodies.h"
#include "../../util/ramlog.h"
#include "../helpers/dblogger.h"
#include "connections.h"

namespace mongo {
    /* decls for connections.h */
    ScopedConn::M& ScopedConn::_map = *(new ScopedConn::M());    
    mutex ScopedConn::mapMutex;
}

namespace mongo { 

    using namespace mongoutils::html;
    using namespace bson;

    static RamLog _rsLog;
    Tee *rsLog = &_rsLog;

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
    public:
        Atomic<RSMember> m;

        string name() { return "ReplSetHealthPoll"; }
        void doWork() { 
            mongo::lastError.initThread();
            {
                RSMember mem = m;
                RSMember old = mem;
                try { 
                    BSONObj info;
                    int theirConfigVersion = -10000;
                    bool ok = requestHeartbeat(theReplSet->name(), mem.h().toString(), info, theReplSet->config().version, theirConfigVersion);
                    mem.lastHeartbeat = time(0); // we set this on any response - we don't get this far if couldn't connect because exception is thrown
                    {
                        be state = info["state"];
                        if( state.ok() )
                            mem.state = (MemberState) state.Int();
                    }
                    if( ok ) {
                        if( mem.upSince == 0 ) {
                            log() << "replSet " << mem.h().toString() << " is now up" << rsLog;
                            mem.upSince = mem.lastHeartbeat;
                        }
                        mem.health = 1.0;
                        mem.lastHeartbeatMsg = "";

                        be cfg = info["config"];
                        if( cfg.ok() ) {
                            ReplSetConfig::receivedNewConfig(cfg.Obj());
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
                    log() << "TODO FINISH code checknewstate" << rsLog;
                    // theReplSet->_mgr.checkNewState();
                }
                sleepsecs(2);
            }
        }
    private:
        void down(RSMember& mem, string msg) {
            mem.health = 0.0;
            if( mem.upSince ) {
                mem.upSince = 0;
                log() << "replSet " << mem.h().toString() << " is now down" << rsLog;
            }
            mem.lastHeartbeatMsg = msg;
        }
    };
    
    string ago(time_t t) { 
        if( t == 0 ) return "";

        time_t x = time(0) - t;
        stringstream s;
        if( x < 180 ) {
            s << x << " sec";
            if( x != 1 ) s << 's';
        }
        else if( x < 3600 ) {
            s.precision(2);
            s << x / 60.0 << " mins";
        }
        else { 
            s.precision(2);
            s << x / 3600.0 << " hrs";
        }
        return s.str();
    }

    void ReplSet::Member::summarizeAsHtml(stringstream& s) const { 
        s << tr();
        {
            stringstream u;
            u << "http://" << m().h().host() << ':' << (m().h().port() + 1000) << "/_replSet";
            s << td( a(u.str(), "", fullName()) );
        }
        double h = m().health;
        bool ok = h > 0;
        s << td(h);
        s << td(ago(m().upSince));
        {
            string h;
            time_t hb = m().lastHeartbeat;
            if( hb == 0 ) h = "never"; 
            else h = ago(hb) + " ago";
            s << td(h);
        }
        s << td(config().votes);
        s << td(ReplSet::stateAsStr(m().state));
        s << td( red(m().lastHeartbeatMsg,!ok) );
        s << _tr();
    }

    string ReplSet::stateAsHtml(MemberState s) { 
        if( s == STARTUP ) return a("", "serving still starting up, or still trying to initiate the set", "STARTUP");
        if( s == PRIMARY ) return a("", "this server thinks it is primary", "PRIMARY");
        if( s == SECONDARY ) return a("", "this server thinks it is a secondary (slave mode)", "SECONDARY");
        if( s == RECOVERING ) return a("", "recovering/resyncing; after recovery usually auto-transitions to secondary", "RECOVERING");
        if( s == FATAL ) return a("", "something bad has occurred and server is not completely offline with regard to the replica set.  fatal error.", "FATAL");
        if( s == STARTUP2 ) return a("", "loaded config, still determining who is primary", "STARTUP2");
        return "";
    }

    string ReplSet::stateAsStr(MemberState s) { 
        if( s == STARTUP ) return "STARTUP";
        if( s == PRIMARY ) return "PRIMARY";
        if( s == SECONDARY ) return "SECONDARY";
        if( s == RECOVERING ) return "RECOVERING";
        if( s == FATAL ) return "FATAL";
        if( s == STARTUP2 ) return "STARTUP2";
        return "";
    }

    extern time_t started;

    void ReplSet::summarizeAsHtml(stringstream& s) const { 
        s << table(0, false);
        s << tr("Set name:", _name);
        s << tr("Majority up:", elect.aMajoritySeemsToBeUp()?"yes":"no" );
        s << _table();

        const char *h[] = {"Member", "Up", 
            "<a title=\"length of time we have been continuously connected to the other member with no reconnects\">cctime</a>", 
            "<a title=\"when this server last received a heartbeat response - includes error code responses\">Last heartbeat</a>", 
            "Votes", "State", "Status", 0};
        s << table(h);

        /* this is to sort the member rows by their ordinal _id, so they show up in the same 
           order on all the different web ui's; that is less confusing for the operator. */
        map<int,string> mp;

        {
            stringstream s;
            /* self row */
            s << tr() << td(_self->fullName()) <<
                td("1") << 
                td(ago(started)) << 
                td("(self)") << 
                td(ToString(_self->config().votes)) << 
                td(stateAsHtml(_myState));
            s << td( _self->lhb() );
            s << _tr();
			mp[_self->m().id()] = s.str();
        }
        Member *m = head();
        while( m ) {
			stringstream s;
            m->summarizeAsHtml(s);
			mp[m->m().id()] = s.str();
            m = m->next();
        }

        for( map<int,string>::const_iterator i = mp.begin(); i != mp.end(); i++ )
            s << i->second;
        s << _table();
    }

    static int repeats(const vector<const char *>& v, int i) { 
        for( int j = i-1; j >= 0 && j+8 > i; j-- ) {
            if( strcmp(v[i]+20,v[j]+20) == 0 ) {
                for( int x = 1; ; x++ ) {
                    if( j+x == i ) return j;
                    if( i+x>=(int) v.size() ) return -1;
                    if( strcmp(v[i+x]+20,v[j+x]+20) ) return -1;
                }
                return -1;
            }
        }
        return -1;
    }

    static string clean(const vector<const char *>& v, int i, string line="") { 
        if( line.empty() ) line = v[i];
        if( i > 0 && strncmp(v[i], v[i-1], 11) == 0 )
            return string("           ") + line.substr(11);
        return v[i];
    }

    static bool isWarning(const char *line) {
        const char *p = strstr(line, "replSet ");
        if( p ) { 
            p += 8;
            return startsWith(p, "warning") || startsWith(p, "error");
        }
        return false;
    }

    void fillRsLog(stringstream& s) {
        bool first = true;
        s << "<pre>\n";
        vector<const char *> v = _rsLog.get();
        for( int i = 0; i < (int)v.size(); i++ ) {
            assert( strlen(v[i]) > 20 );
            int r = repeats(v, i);
            if( r < 0 ) {
                s << red( clean(v,i), isWarning(v[i]) );
            } else {
                stringstream x;
                x << string(v[i], 0, 20);
                int nr = (i-r);
                int last = i+nr-1;
                for( ; r < i ; r++ ) x << '.';
                if( 1 ) { 
                    stringstream r; 
                    if( nr == 1 ) r << "repeat last line";
                    else r << "repeats last " << nr << " lines; ends " << string(v[last]+4,0,15);
                    first = false; s << a("", r.str(), clean(v,i,x.str()));
                }
                else s << x.str();
                s << '\n';
                i = last;
            }
        }
        s << "</pre>\n";
    }

    void ReplSet::summarizeStatus(BSONObjBuilder& b) const { 
        Member *m =_members.head();
        vector<BSONObj> v;

        // add self
        {
            HostAndPort h(getHostName(), cmdLine.port);
            v.push_back( 
                BSON( "name" << h.toString() << "self" << true << 
                      "errmsg" << _self->lhb() ) );
        }

        while( m ) {
            BSONObjBuilder bb;
            bb.append("name", m->fullName());
            bb.append("health", m->m().health);
            bb.append("uptime", (unsigned) (m->m().upSince ? (time(0)-m->m().upSince) : 0));
            bb.appendDate("lastHeartbeat", m->m().lastHeartbeat);
            bb.append("errmsg", m->lhb());
            v.push_back(bb.obj());
            m = m->next();
        }
        b.append("set", name());
        b.appendDate("date", time(0));
        b.append("myState", _myState);
        b.append("members", v);
    }

    void ReplSet::startHealthThreads() {
        /* TODO TODO TODO */
        /*
        Member* m = _members.head();
        while( m ) {
            FeedbackThread *f = new FeedbackThread();
            f->m = m;
            f->go();
            m = m->next();
        }
        */
    }

}

/* todo:
   stop bg job and delete on removefromset
*/
