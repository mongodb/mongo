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
#include "../util/message.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../db/pdfile.h"
#include "gridconfig.h"
#include "../client/model.h"

namespace mongo {

    /* --- Machine --- */

    map<string, Machine*> Machine::machines;

    /* --- Grid --- */

    Grid grid;
    
    DBConfig* Grid::getDBConfig( string database ){
        {
            string::size_type i = database.find( "." );
            if ( i != string::npos )
                database = database.substr( 0 , i );
        }
        
        DBConfig*& cc = _databases[database];
        if ( cc == 0 ) {
            cc = new DBConfig();
            if ( !cc->loadByName(database.c_str()) ) {
                // note here that cc->primary == 0.
                log() << "couldn't find database [" << database << "] in config db" << endl;
            }
        }
        
        return cc;
    }
    
    Machine* Grid::owner(const char *ns, BSONObj& objOrKey) {
        DBConfig *cc = getDBConfig( nsToClient(ns) );
        uassert( string("dbgrid: no config for db for ") + ns , cc );

        if ( !cc->partitioned ) {
            if ( !cc->primary )
                throw UserAssertionException(string("dbgrid: no primary for ")+ns);
            return cc->primary;
        }

        uassert("dbgrid: not implemented 100", false);
        return 0;
    }

} 
