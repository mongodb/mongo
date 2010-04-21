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

/* try to refresh */
void ReplSetConfig::reload(const HostAndPort& h) {
    DBClientConnection conn(false, 0, 20);
    conn._logLevel = 2;
    string err;
    conn.connect(h.toString());
    auto_ptr<DBClientCursor> c = conn.query("admin.system.replset");
    while( c->more() ) { 
        c->nextSafe();
    }
}

}

