// dbinfo.h

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

#include "dbhelpers.h"

namespace mongo {

    class DBContext;

    /* this is an "accessor" class to data held in local.dbinfo.<dbname>

       system.dbinfo contains:
       attributes will be added later.
    */
    class DBInfo {
        string ns;
        DBContext *context;
    public:
        ~DBInfo() {
            delete context;
        }
        DBInfo(const char *db)
        {
            ns = string("local.dbinfo.") + db;
            context = new DBContext(ns);
        }

        BSONObj getDbInfoObj() {
            auto_ptr<Cursor> c = DataFileMgr::findAll(ns.c_str());
            if ( !c->ok() )
                return BSONObj();
            return c->current();
        }

        void dbDropped();
    };

    inline void Database::finishInit() {
        DBInfo i(name.c_str());
    }

} // namespace mongo
