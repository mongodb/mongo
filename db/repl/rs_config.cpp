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
#include "replset.h"
#include "../../client/dbclient.h"
#include "../../util/hostandport.h"
#include "../dbhelpers.h"

using namespace bson;

namespace mongo { 

    void ReplSetConfig::save() { 
        check();
        BSONObj o = asBson();
        Helpers::putSingletonGod("local.system.replset", o, false/*logOp=false; local db so would work regardless...*/);
    }

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

    void ReplSetConfig::clear() { 
        version = -5;
        _ok = false;
    }

    void ReplSetConfig::check() const { 
        uassert(13132,
            "nonmatching repl set name in _id field; check --replSet command line",
            startsWith(cmdLine.replSet, _id + '/'));
        uassert(13133, 
                "replSet config value is not valid",
                members.size() >= 1 && members.size() <= 64 && 
                version > 0);

    }

    void ReplSetConfig::from(BSONObj o) {
        md5 = o.md5();
        _id = o["_id"].String();
        if( o["version"].ok() ) {
            version = o["version"].numberInt();
            uassert(13115, "bad local.system.replset config: version", version > 0);
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
        for( unsigned i = 0; i < members.size(); i++ ) {
            BSONObj mobj = members[i].Obj();
            MemberCfg m;
            try {
                try { 
                    m._id = (int) mobj["_id"].Number();
                } catch(...) { throw "_id must be numeric"; }
                string s;
                try {
                    s = mobj["host"].String();
                    m.h = HostAndPort::fromString(s);
                }
                catch(...) { throw "bad or missing host field?"; }
                m.arbiterOnly = mobj.getBoolField("arbiterOnly");
                try { m.priority = mobj["priority"].Number(); } catch(...) { }
                try { m.votes = (unsigned) mobj["votes"].Number(); } catch(...) { }
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
            uassert(13108, "bad local.system.replset config dups?", ords.count(m._id) == 0 && hosts.count(m.h.toString()) == 0);
            hosts.insert(m.h.toString());
            ords.insert(m._id);
            this->members.push_back(m);
        }
        uassert(13117, "bad local.system.replset config", !_id.empty());
    }

    static inline void configAssert(bool expr) {
        uassert(13122, "bad local.system.replset config", expr);
    }

    ReplSetConfig::ReplSetConfig(BSONObj cfg) { 
        clear();
        from(cfg);
        configAssert( version < 0 /*unspecified*/ || version == 1 );
        version = 1;
        _ok = true;
    }

    ReplSetConfig::ReplSetConfig(const HostAndPort& h) {
        clear();
        int level = 2;
        DEV level = 0;
        log(0) << "replSet load config from: " << h.toString() << rsLog;

        auto_ptr<DBClientCursor> c;
        try {
            DBClientConnection conn(false, 0, 20);
            conn._logLevel = 2;
            string err;
            conn.connect(h.toString());
            version = -4;

            {
                /* first, make sure other node is configured to be a replset. just to be safe. */
                BSONObj cmd = BSON( "replSetHeartbeat" << "preloadconfig?" );
                BSONObj info;
                bool ok = conn.runCommand("admin", cmd, info);
                if( !info["rs"].trueValue() ) { 
                    stringstream ss;
                    ss << "replSet error: member " << h.toString() << " is not in --replSet mode";
                    msgassertedNoTrace(10000, ss.str().c_str()); // not caught as not a user exception - we want it not caught
                }
            }

            version = -3;

            c = conn.query("local.system.replset");
            if( c.get() == 0 )
                return;
            if( !c->more() ) {
                version = -2; /* -2 is a sentinel - see ReplSetConfig::empty() */
                return;
            }
            version = -1;
        }
        catch( UserException& e) { 
            log(level) << "replSet couldn't load config " << h.toString() << ' ' << e.what() << rsLog;
            return;
        }

        BSONObj o = c->nextSafe();
        uassert(13109, "multiple rows in local.system.replset not supported", !c->more());
        from(o);
        _ok = true;
        log(level) << "replSet load ok" << rsLog;
    }

}
