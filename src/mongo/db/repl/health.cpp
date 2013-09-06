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

#include "mongo/db/repl/health.h"

#include "mongo/client/connpool.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/goodies.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/startup_test.h"

namespace mongo {
    /* decls for connections.h */
    ScopedConn::M& ScopedConn::_map = *(new ScopedConn::M());
    mutex ScopedConn::mapMutex("ScopedConn::mapMutex");
}

namespace mongo {

    using namespace mongoutils::html;
    using namespace bson;

    static RamLog * _rsLog = RamLog::get("rs");
    Tee *rsLog = _rsLog;
    extern bool replSetBlind; // for testing

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

    void Member::summarizeMember(stringstream& s) const {
        s << tr();
        {
            stringstream u;
            u << "http://" << h().host() << ':' << (h().port() + 1000) << "/_replSet";
            s << td( a(u.str(), "", fullName()) );
        }
        s << td( id() );
        double h = hbinfo().health;
        bool ok = h > 0;
        s << td(red(str::stream() << h,h == 0));
        s << td(ago(hbinfo().upSince));
        bool never = false;
        {
            string h;
            time_t hb = hbinfo().lastHeartbeat;
            if( hb == 0 ) {
                h = "never";
                never = true;
            }
            else h = ago(hb) + " ago";
            s << td(h);
        }
        s << td(config().votes);
        s << td(config().priority);
        {
            string stateText = state().toString();
            if( _config.hidden )
                stateText += " (hidden)";
            if( ok || stateText.empty() )
                s << td(stateText); // text blank if we've never connected
            else
                s << td( grey(str::stream() << "(was " << state().toString() << ')', true) );
        }
        s << td( grey(hbinfo().lastHeartbeatMsg,!ok) );
        stringstream q;
        q << "/_replSetOplog?_id=" << id();
        s << td( a(q.str(), "", never ? "?" : hbinfo().opTime.toString()) );
        if( hbinfo().skew > INT_MIN ) {
            s << td( grey(str::stream() << hbinfo().skew,!ok) );
        }
        else
            s << td("");
        s << _tr();
    }

    string ReplSetImpl::stateAsHtml(MemberState s) {
        if( s.s == MemberState::RS_STARTUP ) return a("", "serving still starting up, or still trying to initiate the set", "STARTUP");
        if( s.s == MemberState::RS_PRIMARY ) return a("", "this server thinks it is primary", "PRIMARY");
        if( s.s == MemberState::RS_SECONDARY ) return a("", "this server thinks it is a secondary (slave mode)", "SECONDARY");
        if( s.s == MemberState::RS_RECOVERING ) return a("", "recovering/resyncing; after recovery usually auto-transitions to secondary", "RECOVERING");
        if( s.s == MemberState::RS_FATAL ) return a("", "something bad has occurred and server is not completely offline with regard to the replica set.  fatal error.", "FATAL");
        if( s.s == MemberState::RS_STARTUP2 ) return a("", "loaded config, still determining who is primary", "STARTUP2");
        if( s.s == MemberState::RS_ARBITER ) return a("", "this server is an arbiter only", "ARBITER");
        if( s.s == MemberState::RS_DOWN ) return a("", "member is down, slow, or unreachable", "DOWN");
        if( s.s == MemberState::RS_ROLLBACK ) return a("", "rolling back operations to get in sync", "ROLLBACK");
        return "";
    }

    // oplogdiags in web ui
    static void say(stringstream&ss, const bo& op) {
        ss << "<tr>";

        set<string> skip;
        be e = op["ts"];
        if( e.type() == Date || e.type() == Timestamp ) {
            OpTime ot = e._opTime();
            ss << td( time_t_to_String_short( ot.getSecs() ) );
            ss << td( ot.toString() );
            skip.insert("ts");
        }
        else ss << td("?") << td("?");

        e = op["h"];
        if( e.type() == NumberLong ) {
            ss << "<td>" << hex << e.Long() << "</td>\n";
            skip.insert("h");
        }
        else
            ss << td("?");

        ss << td(op["op"].valuestrsafe());
        ss << td(op["ns"].valuestrsafe());
        skip.insert("op");
        skip.insert("ns");

        ss << "<td>";
        for( bo::iterator i(op); i.more(); ) {
            be e = i.next();
            if( skip.count(e.fieldName()) ) continue;
            ss << e.toString() << ' ';
        }
        ss << "</td></tr>\n";
    }

    void ReplSetImpl::_getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const {
        const Member *m = findById(server_id);
        if( m == 0 ) {
            ss << "Error : can't find a member with id: " << server_id << '\n';
            return;
        }

        ss << p("Server : " + m->fullName() + "<br>ns : " + rsoplog );

        //const bo fields = BSON( "o" << false << "o2" << false );
        const bo fields;

        /** todo fix we might want an so timeout here */
        OplogReader reader;

        if (reader.connect(m->fullName()) == false) {
            ss << "couldn't connect to " << m->fullName();
            return;
        }

        reader.query(rsoplog, Query().sort("$natural",1), 20, 0, &fields);
        if ( !reader.haveCursor() ) {
            ss << "couldn't query " << rsoplog;
            return;
        }
        static const char *h[] = {"ts","optime","h","op","ns","rest",0};

        ss << "<style type=\"text/css\" media=\"screen\">"
           "table { font-size:75% }\n"
           // "th { background-color:#bbb; color:#000 }\n"
           // "td,th { padding:.25em }\n"
           "</style>\n";

        ss << table(h, true);
        //ss << "<pre>\n";
        int n = 0;
        OpTime otFirst;
        OpTime otLast;
        OpTime otEnd;
        while( reader.more() ) {
            bo o = reader.next();
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
            reader.query(rsoplog, Query().sort("$natural",-1), 20, 0, &fields);
            if( !reader.haveCursor() ) {
                ss << "couldn't query [2] " << rsoplog;
                return;
            }
            string x;
            bo o = reader.next();
            otEnd = o["ts"]._opTime();
            while( 1 ) {
                stringstream z;
                if( o["ts"]._opTime() == otLast )
                    break;
                say(z, o);
                x = z.str() + x;
                if( !reader.more() )
                    break;
                o = reader.next();
            }
            if( !x.empty() ) {
                ss << "<tr><td>...</td><td>...</td><td>...</td><td>...</td><td>...</td></tr>\n" << x;
                //ss << "\n...\n\n" << x;
            }
        }
        ss << _table();
        ss << p(time_t_to_String_short(time(0)) + " current time");

        if( !otEnd.isNull() ) {
            ss << "<p>Log length in time: ";
            unsigned d = otEnd.getSecs() - otFirst.getSecs();
            double h = d / 3600.0;
            ss.precision(3);
            if( h < 72 )
                ss << h << " hours";
            else
                ss << h / 24.0 << " days";
            ss << "</p>\n";
        }
    }

    void ReplSetImpl::_summarizeAsHtml(stringstream& s) const {
        s << table(0, false);
        s << tr("Set name:", _name);
        s << tr("Majority up:", elect.aMajoritySeemsToBeUp()?"yes":"no" );

        // lag
        const Member *primary = box.getPrimary();
        if (primary != 0 && primary != _self && !iAmArbiterOnly() && !lastOpTimeWritten.isNull()) {
            int lag = primary->hbinfo().opTime.getSecs() - lastOpTimeWritten.getSecs();
            s << tr("Lag: ", str::stream() << lag << " secs");
        }

        s << _table();

        const char *h[] = {"Member",
                           "<a title=\"member id in the replset config\">id</a>",
                           "Up",
                           "<a title=\"length of time we have been continuously connected to the other member with no reconnects (for self, shows uptime)\">cctime</a>",
                           "<a title=\"when this server last received a heartbeat response - includes error code responses\">Last heartbeat</a>",
                           "Votes", "Priority", "State", "Messages",
                           "<a title=\"how up to date this server is.  this value polled every few seconds so actually lag is typically much lower than value shown here.\">optime</a>",
                           "<a title=\"Not replication lag. Clock skew in seconds relative to this server. Informational; server clock variances will make the diagnostics hard to read, but otherwise are benign.\">clock skew</a>",
                           0
                          };
        s << table(h);

        /* this is to sort the member rows by their ordinal _id, so they show up in the same
           order on all the different web ui's; that is less confusing for the operator. */
        map<int,string> mp;

        string myMinValid;
        try {
            readlocktry lk(/*"local.replset.minvalid", */300);
            if( lk.got() ) {
                BSONObj mv;
                if( Helpers::getSingleton("local.replset.minvalid", mv) ) {
                    myMinValid = "minvalid:" + mv["ts"]._opTime().toString();
                }
            }
            else myMinValid = ".";
        }
        catch(...) {
            myMinValid = "exception fetching minvalid";
        }

        const Member *_self = this->_self;
        verify(_self);
        {
            stringstream s;
            /* self row */
            s << tr() << td(_self->fullName() + " (me)") <<
              td(_self->id()) <<
              td("1") <<  //up
              td(ago(cmdLine.started)) <<
              td("") << // last heartbeat
              td(ToString(_self->config().votes)) <<
              td(ToString(_self->config().priority)) <<
              td( stateAsHtml(box.getState()) + (_self->config().hidden?" (hidden)":"") );
            s << td( _hbmsg );
            stringstream q;
            q << "/_replSetOplog?_id=" << _self->id();
            s << td( a(q.str(), myMinValid, theReplSet->lastOpTimeWritten.toString()) );
            s << td(""); // skew
            s << _tr();
            mp[_self->hbinfo().id()] = s.str();
        }
        Member *m = head();
        while( m ) {
            stringstream s;
            m->summarizeMember(s);
            mp[m->hbinfo().id()] = s.str();
            m = m->next();
        }

        for( map<int,string>::const_iterator i = mp.begin(); i != mp.end(); i++ )
            s << i->second;
        s << _table();
    }


    void fillRsLog(stringstream& s) {
        _rsLog->toHTML( s );
    }

    const Member* ReplSetImpl::findById(unsigned id) const {
        if( _self && id == _self->id() ) return _self;

        for( Member *m = head(); m; m = m->next() )
            if( m->id() == id )
                return m;
        return 0;
    }

    Member* ReplSetImpl::getMutableMember(unsigned id) {
        if( _self && id == _self->id() ) return _self;

        for( Member *m = head(); m; m = m->next() )
            if( m->id() == id )
                return m;
        return 0;
    }

    Member* ReplSetImpl::findByName(const std::string& hostname) const {
        if (_self && hostname == _self->fullName()) {
            return _self;
        }

        for (Member *m = head(); m; m = m->next()) {
            if (m->fullName() == hostname) {
                return m;
            }
        }

        return NULL;
    }

    const OpTime ReplSetImpl::lastOtherOpTime() const {
        OpTime closest(0,0);

        for( Member *m = _members.head(); m; m=m->next() ) {
            if (!m->hbinfo().up()) {
                continue;
            }

            if (m->hbinfo().opTime > closest) {
                closest = m->hbinfo().opTime;
            }
        }

        return closest;
    }

    const OpTime ReplSetImpl::lastOtherElectableOpTime() const {
        OpTime closest(0,0);

        for( Member *m = _members.head(); m; m=m->next() ) {
            if (!m->hbinfo().up()) {
                continue;
            }

            if (m->hbinfo().opTime > closest && m->config().potentiallyHot()) {
                log() << m->fullName() << " is now closest at "
                      <<  m->hbinfo().opTime << endl;
                closest = m->hbinfo().opTime;
            }
        }

        return closest;
    }

    void ReplSetImpl::_summarizeStatus(BSONObjBuilder& b) const {
        vector<BSONObj> v;

        const Member *_self = this->_self;
        verify( _self );

        MemberState myState = box.getState();

        // add self
        {
            BSONObjBuilder bb;
            bb.append("_id", (int) _self->id());
            bb.append("name", _self->fullName());
            bb.append("health", 1.0);
            bb.append("state", (int)myState.s);
            bb.append("stateStr", myState.toString());
            bb.append("uptime", (unsigned)(time(0) - cmdLine.started));
            if (!_self->config().arbiterOnly) {
                bb.appendTimestamp("optime", lastOpTimeWritten.asDate());
                bb.appendDate("optimeDate", lastOpTimeWritten.getSecs() * 1000LL);
            }

            int maintenance = _maintenanceMode;
            if (maintenance) {
                bb.append("maintenanceMode", maintenance);
            }

            if (theReplSet) {
                string s = theReplSet->hbmsg();
                if( !s.empty() )
                    bb.append("errmsg", s);
            }
            bb.append("self", true);
            v.push_back(bb.obj());
        }

        Member *m =_members.head();
        while( m ) {
            BSONObjBuilder bb;
            bb.append("_id", (int) m->id());
            bb.append("name", m->fullName());
            double h = m->hbinfo().health;
            bb.append("health", h);
            bb.append("state", (int) m->state().s);
            if( h == 0 ) {
                // if we can't connect the state info is from the past and could be confusing to show
                bb.append("stateStr", "(not reachable/healthy)");
            }
            else {
                bb.append("stateStr", m->state().toString());
            }
            bb.append("uptime", (unsigned) (m->hbinfo().upSince ? (time(0)-m->hbinfo().upSince) : 0));
            if (!m->config().arbiterOnly) {
                bb.appendTimestamp("optime", m->hbinfo().opTime.asDate());
                bb.appendDate("optimeDate", m->hbinfo().opTime.getSecs() * 1000LL);
            }
            bb.appendTimeT("lastHeartbeat", m->hbinfo().lastHeartbeat);
            bb.appendTimeT("lastHeartbeatRecv", m->hbinfo().lastHeartbeatRecv);
            bb.append("pingMs", m->hbinfo().ping);
            string s = m->lhb();
            if( !s.empty() )
                bb.append("lastHeartbeatMessage", s);

            if (m->hbinfo().authIssue) {
                bb.append("authenticated", false);
            }

            string syncingTo = m->hbinfo().syncingTo;
            if (!syncingTo.empty()) {
                bb.append("syncingTo", syncingTo);
            }

            v.push_back(bb.obj());
            m = m->next();
        }
        sort(v.begin(), v.end());
        b.append("set", name());
        b.appendTimeT("date", time(0));
        b.append("myState", myState.s);
        const Member *syncTarget = replset::BackgroundSync::get()->getSyncTarget();
        if ( syncTarget &&
            (myState != MemberState::RS_PRIMARY) &&
            (myState != MemberState::RS_SHUNNED) ) {
            b.append("syncingTo", syncTarget->fullName());
        }
        b.append("members", v);
        if( replSetBlind )
            b.append("blind",true); // to avoid confusion if set...normally never set except for testing.
    }

    static struct Test : public StartupTest {
        void run() {
            HealthOptions a,b;
            verify( a == b );
            verify( a.isDefault() );
        }
    } test;

}
