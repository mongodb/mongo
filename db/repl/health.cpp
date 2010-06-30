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
    mutex ScopedConn::mapMutex("ScopedConn::mapMutex");
}

namespace mongo { 

    using namespace mongoutils::html;
    using namespace bson;

    static RamLog _rsLog;
    Tee *rsLog = &_rsLog;

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

    void Member::summarizeAsHtml(stringstream& s) const { 
        s << tr();
        {
            stringstream u;
            u << "http://" << h().host() << ':' << (h().port() + 1000) << "/_replSet";
            s << td( a(u.str(), "", fullName()) );
        }
        double h = hbinfo().health;
        bool ok = h > 0;
        s << td(h);
        s << td(ago(hbinfo().upSince));
        {
            string h;
            time_t hb = hbinfo().lastHeartbeat;
            if( hb == 0 ) h = "never"; 
            else h = ago(hb) + " ago";
            s << td(h);
        }
        s << td(config().votes);
        s << td( grey(ReplSet::stateAsStr(state()), !ok) );
        s << td( grey(hbinfo().lastHeartbeatMsg,!ok) );
        stringstream q;
        q << "/_replSetOplog?" << id();
        s << td( a(q.str(), "", hbinfo().opTime.toString()) );
        s << _tr();
    }

    string ReplSetImpl::stateAsHtml(MemberState s) { 
        if( s == STARTUP ) return a("", "serving still starting up, or still trying to initiate the set", "STARTUP");
        if( s == PRIMARY ) return a("", "this server thinks it is primary", "PRIMARY");
        if( s == SECONDARY ) return a("", "this server thinks it is a secondary (slave mode)", "SECONDARY");
        if( s == RECOVERING ) return a("", "recovering/resyncing; after recovery usually auto-transitions to secondary", "RECOVERING");
        if( s == FATAL ) return a("", "something bad has occurred and server is not completely offline with regard to the replica set.  fatal error.", "FATAL");
        if( s == STARTUP2 ) return a("", "loaded config, still determining who is primary", "STARTUP2");
        return "";
    }

    string ReplSetImpl::stateAsStr(MemberState s) { 
        if( s == STARTUP ) return "STARTUP";
        if( s == PRIMARY ) return "PRIMARY";
        if( s == SECONDARY ) return "SECONDARY";
        if( s == RECOVERING ) return "RECOVERING";
        if( s == FATAL ) return "FATAL";
        if( s == STARTUP2 ) return "STARTUP2";
        return "";
    }

    extern time_t started;

    static void say(stringstream&ss, const bo& op) {
        ss << op.toString() << '\n';
    }

    void ReplSetImpl::_getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const { 
        Member *m = findById(server_id);
        if( m == 0 ) { 
            ss << "Error : can't find a member with id: " << server_id << '\n';
            return;
        }

        ss << p("Server : " + m->fullName() );

        const bo fields = BSON( "o" << false << "o2" << false );

        ScopedConn conn(m->fullName());        

        auto_ptr<DBClientCursor> c = conn->query(rsoplog, Query().sort("$natural",1), 20, 0, &fields);
        ss << "<pre>\n";
        int n = 0;
        OpTime otFirst;
        OpTime otLast;
        OpTime otEnd;
        while( c->more() ) {
            bo o = c->next();
            otLast = o["ts"]._opTime();
            if( otFirst.isNull() ) 
                otFirst = otLast;
            say(ss, o);
            n++;            
        }
        if( n == 0 ) {
            ss << rsoplog << " is empty\n";
        }
        else { 
            auto_ptr<DBClientCursor> c = conn->query(rsoplog, Query().sort("$natural",-1), 20, 0, &fields);
            string x;
            bo o = c->next();
            otEnd = o["ts"]._opTime();
            while( 1 ) {
                stringstream z;
                if( o["ts"]._opTime() == otLast ) 
                    break;
                say(z, o);
                x = z.str() + x;
                if( !c->more() )
                    break;
                bo o = c->next();
            }
            if( !x.empty() ) {
                ss << "\n...\n\n" << x;
            }
        }
        ss << "</pre>\n";
        if( !otEnd.isNull() )
            ss << "<p>Log length in time: " << otEnd.getSecs() - otFirst.getSecs() << " secs</p>\n";
    }

    void ReplSetImpl::_summarizeAsHtml(stringstream& s) const { 
        s << table(0, false);
        s << tr("Set name:", _name);
        s << tr("Majority up:", elect.aMajoritySeemsToBeUp()?"yes":"no" );
        s << _table();

        const char *h[] = {"Member", "Up", 
            "<a title=\"length of time we have been continuously connected to the other member with no reconnects\">cctime</a>", 
            "<a title=\"when this server last received a heartbeat response - includes error code responses\">Last heartbeat</a>", 
            "Votes", "State", "Status", 
            "<a title=\"how up to date this server is; write operations are sequentially numbered\">opord</a>", 
            0};
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
            stringstream q;
            q << "/_replSetOplog?" << _self->id();
            s << td( a(q.str(), "", theReplSet->lastOpTimeWritten.toString()) );
            s << _tr();
			mp[_self->hbinfo().id()] = s.str();
        }
        Member *m = head();
        while( m ) {
			stringstream s;
            m->summarizeAsHtml(s);
			mp[m->hbinfo().id()] = s.str();
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

    /*static bool isWarning(const char *line) {
        const char *p = strstr(line, "replSet ");
        if( p ) { 
            p += 8;
            return startsWith(p, "warning") || startsWith(p, "error");
        }
        return false;
    }*/
    static string color(string line) { 
        string s = str::after(line, "replSet ");
        if( str::startsWith(s, "warning") || startsWith(s, "error") )
            return red(line);
        if( str::startsWith(s, "info") ) {
            if( str::endsWith(s, " up\n") )
                return green(line);
            if( str::endsWith(s, " down\n") )
                return yellow(line);
            return blue(line);
        }

        return line;
    }

    void fillRsLog(stringstream& s) {
        bool first = true;
        s << "<pre>\n";
        vector<const char *> v = _rsLog.get();
        for( int i = 0; i < (int)v.size(); i++ ) {
            assert( strlen(v[i]) > 20 );
            int r = repeats(v, i);
            if( r < 0 ) {
                s << color( clean(v,i) );
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

    Member* ReplSetImpl::findById(unsigned id) const { 
        if( id == _self->id() ) return _self;
        for( Member *m = head(); m; m = m->next() )
            if( m->id() == id ) 
                return m;
        return 0;
    }

    void ReplSetImpl::_summarizeStatus(BSONObjBuilder& b) const { 
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
            bb.append("health", m->hbinfo().health);
            bb.append("uptime", (unsigned) (m->hbinfo().upSince ? (time(0)-m->hbinfo().upSince) : 0));
            bb.appendDate("lastHeartbeat", m->hbinfo().lastHeartbeat);
            bb.append("errmsg", m->lhb());
            v.push_back(bb.obj());
            m = m->next();
        }
        b.append("set", name());
        b.appendDate("date", time(0));
        b.append("myState", _myState);
        b.append("members", v);
    }

}
