/* griddatabase.h

   The grid database is where we get:
   - name of each shard
   - "home" shard for each database
*/

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

#pragma once

#include "shard.h"

class GridDatabase {
public:
    DBClientWithCommands *conn;
//    DBClientPaired conn;
    enum { Port = 27016 }; /* standard port # for a grid db */
    GridDatabase();
    ~GridDatabase();
    string toString() {
        return conn->toString();
    }

    /* call at startup, this will initiate connection to the grid db */
    void init();
};
extern GridDatabase gridDatabase;

