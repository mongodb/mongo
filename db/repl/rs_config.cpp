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

#include "stdafx.h"
#include "rs_config.h"
#include "replset.h"
#include "../../client/dbclient.h"
#include "../../util/hostandport.h"

namespace mongo { 

    BSONObj ReplSetConfig::bson() const { 
        return BSONObjBuilder().append("_id", _id).append("version", version).obj();
    }

    static inline void mchk(bool expr) {
        uassert(13126, "bad Member config", expr);
    }

    void ReplSetConfig::Member::check() const {
        mchk(priority >= 0 && priority <= 1000);
        mchk(votes >= 0 && votes <= 100);
    }

    void ReplSetConfig::clear() { 
        version = -5;
        _ok = false;
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
            if( settings["connRetries"].ok() )
                healthOptions.connRetries = settings["connRetries"].numberInt();
            if( settings["heartbeatSleep"].ok() )
                healthOptions.heartbeatSleepMillis = (unsigned) (settings["heartbeatSleep"].Number() * 1000);
            if( settings["heartbeatTimeout"].ok() )
                healthOptions.heartbeatTimeoutMillis = (unsigned) (settings["heartbeatTimeout"].Number() * 1000);
            healthOptions.check();
        }

        set<string> hosts;
        vector<BSONElement> members = o["members"].Array();
        for( unsigned i = 0; i < members.size(); i++ ) {
            BSONObj mobj = members[i].Obj();
            Member m;
            string s = mobj["host"].String();
            try {
                m.h = HostAndPort::fromString(s);
                m.arbiterOnly = mobj.getBoolField("arbiterOnly");
                try { m.priority = mobj["priority"].Number(); } catch(...) { }
                try { m.votes = (unsigned) mobj["votes"].Number(); } catch(...) { }
                m.check();
            }
            catch(...) { 
                uassert(13107, "bad local.system.replset config", false);
            }
            uassert(13108, "bad local.system.replset config dups?", hosts.count(m.h.toString()) == 0);
            hosts.insert(m.h.toString());
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
        log(0) << "replSet load config from: " << h.toString() << endl;

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
                cout << h.toString() << " " << ok << " " << info.toString() << endl;
                if( !info["rs"].trueValue() ) { 
                    stringstream ss;
                    ss << "replSet error: member " << h.toString() << " is not in --replSet mode";
                    msgassertedNoTrace(10000, ss.str().c_str()); // not caught as not a user exception - we want it not caught
                }
            }

            version = -3;

            c = conn.query("local.system.replset");
            if( !c->more() ) {
                version = -2; /* -2 is a sentinel - see ReplSetConfig::empty() */
                return;
            }
            version = -1;
        }
        catch( UserException& e) { 
            log(level) << "replSet couldn't load config " << h.toString() << ' ' << e.what() << endl;
            return;
        }

        BSONObj o = c->nextSafe();
        uassert(13109, "multiple rows in local.system.replset not supported", !c->more());
        from(o);
        _ok = true;
        log(level) << "replSet load ok" << endl;
    }

}
