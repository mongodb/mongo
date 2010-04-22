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
#include "../../client/dbclient.h"
#include "../../util/hostandport.h"

namespace mongo { 

    void ReplSetConfig::from(BSONObj o) {
        md5 = o.md5();
        _id = o["_id"].String();
        int v = o["version"].numberInt();
        uassert(13115, "bad local.system.replset config: version", v > 0);
        version = v;

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
                BSONElement pri = mobj["priority"];
                m.priority = pri.ok() ? pri.number() : 1.0;
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

    ReplSetConfig::ReplSetConfig(const HostAndPort& h) : version(-4) {
        int level = 2;
        DEV level = 0;
        _ok = false;
        log(0) << "replSet load config from: " << h.toString() << endl;

        auto_ptr<DBClientCursor> c;
        try {
            DBClientConnection conn(false, 0, 20);
            conn._logLevel = 2;
            string err;
            conn.connect(h.toString());
            version = -3;
            c = conn.query("local.system.replset");
            if( !c->more() ) {
                version = -2;
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
