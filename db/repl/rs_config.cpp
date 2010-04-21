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

ReplSetConfig::ReplSetConfig(const HostAndPort& h) {
    DBClientConnection conn(false, 0, 20);
    conn._logLevel = 2;
    string err;
    conn.connect(h.toString());
    auto_ptr<DBClientCursor> c = conn.query("admin.replset");
    set<string> hosts;
    BSONObj o = c->nextSafe();
    uassert(13109, "multiple rows in admin.replset not supported", !c->more());

    { 
        _id = o.getStringField("_id");
        version = o.getIntField("version");
        uassert(13115, "bad admin.replset config: version", version > 0);

        BSONObj settings = o.getObjectField("settings");
        if( !settings.isEmpty() ) {
            if( settings.hasField("connRetries") )
                healthOptions.connRetries = settings.getIntField("connRetries");
            if( settings.hasField("heartbeatSleep") )
                healthOptions.heartbeatSleepMillis = (unsigned) (settings["heartbeatSleep"].number() * 1000);
            if( settings.hasField("heartbeatTimeout" ) )
                healthOptions.heartbeatTimeoutMillis = (unsigned) (settings["heartbeatTimeout"].number() * 1000);
            healthOptions.check();
        }

        BSONObjIterator i(o.getObjectField("members"));
        while( i.more() ) {
            BSONObj memb = i.next().embeddedObject();
            Member m;
            string s = memb.getStringField("host");
            try {
                m.h = HostAndPort::fromString(s);
                m.arbiterOnly = memb.getBoolField("arbiterOnly");
                m.priority = memb.hasField("priority") ? memb["priority"].number() : 1.0;
            }
            catch(...) { 
                uassert(13107, "bad admin.replset config", false);
            }
            uassert(13108, "bad admin.replset config", hosts.count(m.h.toString()) == 0);
            hosts.insert(m.h.toString());
        }
    }
    uassert(13117, "bad admin.replset config", !_id.empty());
}

}

