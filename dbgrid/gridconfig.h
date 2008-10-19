// gridconfig.h

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

/* This file is things related to the "grid configuration":
   - what machines make up the db component of our cloud
   - where various ranges of things live
*/

#pragma once

class GridDB {
public:
    enum { Port = 30000 };
    GridDB();
};

/* Machine is the concept of a host that runs the db process.
*/
class Machine { 
public:
    enum { Port = 27018 /* standard port # for dbs that are downstream of a dbgrid */
    };
};

typedef map<string,Machine*> ObjLocs;

class GridConfig { 
    ObjLocs loc;
public:
    /* return which machine "owns" the object in question -- ie which partition 
       we should go to. 
       
       threadsafe.
    */
    Machine* owner(const char *ns, JSObj& objOrKey);

    GridConfig();
};

extern GridConfig gridConfig;
