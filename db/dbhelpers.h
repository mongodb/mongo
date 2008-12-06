// dbhelpers.h

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

/* db helpers are helper functions and classes that let us easily manipulate the local 
   database instance.
*/

#pragma once


/* Get/put the first object from a collection.  Generally only useful if the collection 
   only ever has a single object -- which is a "singleton collection".

   You do not need to set the database before calling.

   Returns: true if object exists. 
*/
bool getSingleton(const char *ns, BSONObj& result);
void putSingleton(const char *ns, BSONObj obj);


/* Set database we want to use, then, restores when we finish (are out of scope)
   Note this is also helpful if an exception happens as the state if fixed up.
*/
class DBContext { 
    Database *old;
public:
    DBContext(const char *ns) { 
        old = database;
        setClientTempNs(ns);
    }
    DBContext(string ns) { 
        old = database;
        setClientTempNs(ns.c_str());
    }
    ~DBContext() { database = old; }
};
