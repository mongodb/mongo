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
        version = o["version"].numberInt();
        uassert(13115, "bad admin.replset config: version", version > 0);

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
                uassert(13107, "bad admin.replset config", false);
            }
            uassert(13108, "bad admin.replset config dups?", hosts.count(m.h.toString()) == 0);
            hosts.insert(m.h.toString());
        }
        uassert(13117, "bad admin.replset config", !_id.empty());
    }

    static inline void configAssert(bool expr) {
        uassert(13122, "bad admin.replset config", expr);
    }

    ReplSetConfig::ReplSetConfig(const HostAndPort& h) {
        DBClientConnection conn(false, 0, 20);
        conn._logLevel = 2;
        string err;
        conn.connect(h.toString());
        auto_ptr<DBClientCursor> c = conn.query("admin.replset");
        configAssert(c->more());
        BSONObj o = c->nextSafe();
        uassert(13109, "multiple rows in admin.replset not supported", !c->more());
        from(o);
    }

}
