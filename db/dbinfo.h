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

namespace mongo {

    /* this is an "accessor" class to data held in local.dbinfo.<dbname>

       system.dbinfo contains:
       attributes will be added later.
    */
    class DBInfo {
        string ns;
        Database *dbold;
    public:
        ~DBInfo() {
            database = dbold;
        }
        DBInfo(const char *db) {
            dbold = database;
            ns = string("local.dbinfo.") + db;
            setClientTempNs(ns.c_str());
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
