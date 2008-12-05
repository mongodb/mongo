// dbhelpers.cpp

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
#include "db.h"
#include "dbhelpers.h"
#include "query.h"

/* Get the first object from a collection.  Generally only useful if the collection 
   only ever has a single object -- which is a "singleton collection.

   Returns: true if object exists. 
*/
bool getSingleton(const char *ns, BSONObj& result) {
    DBContext context(ns);

    auto_ptr<Cursor> c = DataFileMgr::findAll(ns);
    if( !c->ok() )
        return false;

    result = c->current();
    return true;
}

void putSingleton(const char *ns, BSONObj obj) {
    DBContext context(ns);
    stringstream ss;
    updateObjects(ns, obj, /*pattern=*/emptyObj, /*upsert=*/true, ss);
}
