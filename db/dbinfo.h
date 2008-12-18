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

/* this is an "accessor" class to data held in local.dbinfo.<dbname>

   system.dbinfo contains:

       { haveLogged : true }

         haveLogged -- if true, we have already logged events to the oplog for this 
           database.  missing implies false.

   other attributes will be added later.

   Note that class Database caches the DBInfo::haveLogged() value to keep things fast.
*/
class DBInfo { 
    string ns;
    Database *dbold;
public:
    ~DBInfo() { database = dbold; }
    DBInfo(const char *db) { 
        dbold = database;
        ns = string("local.dbinfo.") + db;
        setClientTempNs(ns.c_str());
    }

    BSONObj getDbInfoObj() { 
        auto_ptr<Cursor> c = DataFileMgr::findAll(ns.c_str());
        if( !c->ok() )
            return BSONObj();
        return c->current();
    }

    bool haveLogged() { 
        return getDbInfoObj().getBoolField("haveLogged");
    }

    void setHaveLogged();
    void dbDropped();
};

inline void Database::setHaveLogged() { 
    if( _haveLogged ) return;
    DBInfo i(name.c_str());
    i.setHaveLogged();
    _haveLogged = true;
}

inline void Database::finishInit() { 
    DBInfo i(name.c_str());
    _haveLogged = i.haveLogged();
}
