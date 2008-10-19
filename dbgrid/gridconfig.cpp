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

static boost::mutex loc_mutex;
static boost::mutex griddb_mutex;
GridDB gridDB;
GridConfig gridConfig;

GridDB::GridDB() { 
}

GridConfig::GridConfig() { 
}

/*threadsafe*/
Machine* GridConfig::owner(const char *ns, JSObj& objOrKey) {
    {
        boostlock lk(loc_mutex);

        ObjLocs::iterator i = loc.find(ns);
        if( i != loc.end() ) { 
            return i->second;
        }
        i = loc.find(nsToClient(ns));
        if( i != loc.end() ) { 
            return i->second;
        }
    }

    return 0;
}
