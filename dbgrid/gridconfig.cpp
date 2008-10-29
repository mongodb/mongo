// gridconfig.cpp

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
#include "../grid/message.h"
#include "../util/unittest.h"
#include "database.h"
#include "connpool.h"
#include "../db/pdfile.h"
#include "gridconfig.h"
#include "../client/model.h"

/* --- Machine --- */

map<string, Machine*> Machine::machines;

/* --- GridConfig --- */

//static boost::mutex loc_mutex;
Grid grid;

ClientConfig* GridConfig::getClientConfig(string client) { 
    ClientConfig*& cc = clients[client];
    if( cc == 0 ) { 
        cc = new ClientConfig();
        if( !cc->loadByName(client.c_str()) ) { 
            log() << "couldn't find client " << client << " in grid db" << endl;
            // note here that cc->primary == 0.
        }
    }
    return cc;
}

/* --- Grid --- */

Machine* Grid::owner(const char *ns, BSONObj& objOrKey) {
    ClientConfig *cc = gc.getClientConfig( nsToClient(ns) );
    if( cc == 0 ) {
        throw UserAssertionException(
            string("dbgrid: no config for db for ") + ns);
    }

    if( !cc->partitioned ) { 
        if( !cc->primary )
            throw UserAssertionException(string("dbgrid: no primary for ")+ns);
        return cc->primary;
    }

    uassert("dbgrid: not implemented 100", false);
    return 0;
}
