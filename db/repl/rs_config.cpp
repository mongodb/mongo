// rs_config.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "rs.h"
#include "../../client/dbclient.h"
#include "../../client/syncclusterconnection.h"
#include "../../util/hostandport.h"
#include "../dbhelpers.h"
#include "connections.h"
#include "../oplog.h"

using namespace bson;

namespace mongo { 

    void logOpInitiate(const bo&);

    list<HostAndPort> ReplSetConfig::otherMemberHostnames() const { 
        list<HostAndPort> L;
        for( vector<MemberCfg>::const_iterator i = members.begin(); i != members.end(); i++ ) {
            if( !i->h.isSelf() )
                L.push_back(i->h);
        }
        return L;
    }

    /* comment MUST only be set when initiating the set by the initiator */
    void ReplSetConfig::saveConfigLocally(bo comment) { 
        check();
        log() << "replSet info saving a newer config version to local.system.replset" << rsLog;
        MemoryMappedFile::flushAll(true);
        { 
            writelock lk("");
            //theReplSet->lastOpTimeWritten = ??;
            //rather than above, do a logOp()? probably
            BSONObj o = asBson();
            Helpers::putSingletonGod(rsConfigNs.c_str(), o, false/*logOp=false; local db so would work regardless...*/);
            if( !comment.isEmpty() )
                logOpInitiate(comment);
            MemoryMappedFile::flushAll(true);
        }
        DEV log() << "replSet saveConfigLocally done" << rsLog;
    }
    
    /*static*/ 
    /*void ReplSetConfig::receivedNewConfig(BSONObj cfg) { 
        if( theReplSet )
            return; // this is for initial setup only, so far. todo

        ReplSetConfig c(cfg);

        writelock lk("admin.");
        if( theReplSet ) 
            return;
        c.saveConfigLocally(bo());
    }*/

    bo ReplSetConfig::MemberCfg::asBson() const { 
        bob b;
        b << "_id" << _id;
        b.append("host", h.toString());
        if( votes != 1 ) b << "votes" << votes;
        if( priority != 1.0 ) b << "priority" << priority;
        if( arbiterOnly ) b << "arbiterOnly" << true;
        return b.obj();
    }

    bo ReplSetConfig::asBson() const { 
        bob b;
        b.append("_id", _id).append("version", version);
        if( !ho.isDefault() || !getLastErrorDefaults.isEmpty() ) {
            bob settings;
            if( !ho.isDefault() )
                settings << "heartbeatConnRetries " << ho.heartbeatConnRetries  << 
                             "heartbeatSleep" << ho.heartbeatSleepMillis / 1000 << 
                             "heartbeatTimeout" << ho.heartbeatTimeoutMillis / 1000;
            if( !getLastErrorDefaults.isEmpty() )
                settings << "getLastErrorDefaults" << getLastErrorDefaults;
            b << "settings" << settings.obj();
        }

        BSONArrayBuilder a;
        for( unsigned i = 0; i < members.size(); i++ )
            a.append( members[i].asBson() );
        b.append("members", a.arr());

        return b.obj();
    }

    static inline void mchk(bool expr) {
        uassert(13126, "bad Member config", expr);
    }

    void ReplSetConfig::MemberCfg::check() const{ 
        mchk(_id >= 0 && _id <= 255);
        mchk(priority >= 0 && priority <= 1000);
        mchk(votes >= 0 && votes <= 100);
    }

    /*static*/ bool ReplSetConfig::legalChange(const ReplSetConfig& o, const ReplSetConfig& n, string& errmsg) { 
        if( o._id != n._id ) { 
            errmsg = "set name may not change"; 
            return false;
        }
        /* TODO : wonder if we need to allow o.version < n.version only, which is more lenient.
                  if someone had some intermediate config this node doesnt have, that could be 
                  necessary.  but then how did we become primary?  so perhaps we are fine as-is.
                  */
        if( o.version + 1 != n.version ) { 
            errmsg = "version number wrong";
            return false;
        }

        /* TODO : MORE CHECKS HERE */

        cout << "TODO : don't allow removal of a node until we handle it at the removed node end." << endl;
        // we could change its votes to zero perhaps instead as a short term...

        return true;
    }

    void ReplSetConfig::clear() { 
        version = -5;
        _ok = false;
    }

    void ReplSetConfig::check() const { 
        uassert(13132,
            "nonmatching repl set name in _id field; check --replSet command line",
            startsWith(cmdLine.replSet, _id + '/'));
        uassert(13308, "replSet bad config version #", version > 0);
        uassert(13133, "replSet bad config no members", members.size() >= 1);
        uassert(13309, "replSet bad config maximum number of members is 7 (for now)", members.size() <= 7);
    }

    void ReplSetConfig::from(BSONObj o) {
        md5 = o.md5();
        _id = o["_id"].String();
        if( o["version"].ok() ) {
            version = o["version"].numberInt();
            uassert(13115, "bad " + rsConfigNs + " config: version", version > 0);
        }

        if( o["settings"].ok() ) {
            BSONObj settings = o["settings"].Obj();
            if( settings["heartbeatConnRetries "].ok() )
                ho.heartbeatConnRetries  = settings["heartbeatConnRetries "].numberInt();
            if( settings["heartbeatSleep"].ok() )
                ho.heartbeatSleepMillis = (unsigned) (settings["heartbeatSleep"].Number() * 1000);
            if( settings["heartbeatTimeout"].ok() )
                ho.heartbeatTimeoutMillis = (unsigned) (settings["heartbeatTimeout"].Number() * 1000);
            ho.check();
            try { getLastErrorDefaults = settings["getLastErrorDefaults"].Obj(); } catch(...) { }
        }

        set<string> hosts;
        set<int> ords;
        vector<BSONElement> members;
        try {
            members = o["members"].Array();
        }
        catch(...) {
            uasserted(13131, "replSet error parsing (or missing) 'members' field in config object");
        }

        int localhosts = 0;
        for( unsigned i = 0; i < members.size(); i++ ) {
            BSONObj mobj = members[i].Obj();
            MemberCfg m;
            try {
                try { 
                    m._id = (int) mobj["_id"].Number();
                } catch(...) { 
                    /* TODO: use of string exceptions may be problematic for reconfig case! */
                    throw "_id must be numeric"; 
                }
                string s;
                try {
                    s = mobj["host"].String();
                    m.h = HostAndPort(s);
                }
                catch(...) { 
                    throw string("bad or missing host field? ") + mobj.toString();
                }
                if( m.h.isLocalHost() ) 
                    localhosts++;
                m.arbiterOnly = mobj.getBoolField("arbiterOnly");
                if( mobj.hasElement("priority") )
                    m.priority = mobj["priority"].Number();
                if( mobj.hasElement("votes") )
                    m.votes = (unsigned) mobj["votes"].Number();
                m.check();
            }
            catch( const char * p ) { 
                log() << "replSet cfg parsing exception for members[" << i << "] " << p << rsLog;
                stringstream ss;
                ss << "replSet members[" << i << "] " << p;
                uassert(13107, ss.str(), false);
            }
            catch(DBException& e) { 
                log() << "replSet cfg parsing exception for members[" << i << "] " << e.what() << rsLog;
                stringstream ss;
                ss << "replSet members[" << i << "] bad config object";
                uassert(13135, ss.str(), false);
            }
            uassert(13108, "bad " + rsConfigNs + " config dups?", ords.count(m._id) == 0 && hosts.count(m.h.toString()) == 0);
            hosts.insert(m.h.toString());
            ords.insert(m._id);
            this->members.push_back(m);
        }
        uassert(13393, "can't use localhost in repl set member names except when using it for all members", localhosts == 0 || localhosts == members.size());
        uassert(13117, "bad " + rsConfigNs + " config", !_id.empty());
    }

    static inline void configAssert(bool expr) {
        uassert(13122, "bad repl set config?", expr);
    }

    ReplSetConfig::ReplSetConfig(BSONObj cfg) { 
        clear();
        from(cfg);
        configAssert( version < 0 /*unspecified*/ || (version >= 1 && version <= 5000) );
        if( version < 1 )
            version = 1;
        _ok = true;
    }

    ReplSetConfig::ReplSetConfig(const HostAndPort& h) {
        clear();
        int level = 2;
        DEV level = 0;
        //log(0) << "replSet load config from: " << h.toString() << rsLog;

        auto_ptr<DBClientCursor> c;
        int v = -5;
        try {
            if( h.isSelf() ) {
                ;
            }
            else {
                /* first, make sure other node is configured to be a replset. just to be safe. */
                size_t sl = cmdLine.replSet.find('/');
                assert( sl != string::npos );
                string setname = cmdLine.replSet.substr(0, sl);
                BSONObj cmd = BSON( "replSetHeartbeat" << setname );
                int theirVersion;
                BSONObj info;
                bool ok = requestHeartbeat(setname, h.toString(), info, -2, theirVersion);
                if( info["rs"].trueValue() ) { 
                    // yes, it is a replicate set, although perhaps not yet initialized
                }
                else {
                    if( !ok ) {
                        log() << "replSet TEMP !ok heartbeating " << h.toString() << " on cfg load" << rsLog;
                        if( !info.isEmpty() ) 
                            log() << "replSet info " << h.toString() << " : " << info.toString() << rsLog;
                        return;
                    }
                    { 
                        stringstream ss;
                        ss << "replSet error: member " << h.toString() << " is not in --replSet mode";
                        msgassertedNoTrace(13260, ss.str().c_str()); // not caught as not a user exception - we want it not caught
                        //for python err# checker: uassert(13260, "", false);
                    }
                }
            }

            v = -4;
            ScopedConn conn(h.toString());
            v = -3;
            c = conn->query(rsConfigNs);
            if( c.get() == 0 ) {
                version = v; return;
            }
            if( !c->more() ) {
                version = EMPTYCONFIG;
                return;
            }
            version = -1;
        }
        catch( DBException& e) { 
            version = v;
            log(level) << "replSet load config couldn't get from " << h.toString() << ' ' << e.what() << rsLog;
            return;
        }

        BSONObj o = c->nextSafe();
        uassert(13109, "multiple rows in " + rsConfigNs + " not supported", !c->more());
        from(o);
        _ok = true;
        log(level) << "replSet load config ok from " << (h.isSelf() ? "self" : h.toString()) << rsLog;
    }

}
