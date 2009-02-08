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
#include "../client/model.h"
#include "../db/pdfile.h"
#include "gridconfig.h"
#include "configserver.h"

namespace mongo {

    /* --- DBConfig --- */

    string DBConfig::modelServer() {
        return configServer.modelServer();
    }

    bool DBConfig::partitioned( const NamespaceString& ns ){
        if ( ! _partitioned )
            return false;
        uassert( "don't know what to do!" , 0 );
        return 0;
    }

    string DBConfig::getServer( const NamespaceString& ns ){
        if ( partitioned( ns ) )
            return 0;
        
        uassert( "no primary!" , _primary.size() );
        return _primary;
    }

    void DBConfig::serialize(BSONObjBuilder& to){
        to.append("name", _name);
        to.appendBool("partitioned", _partitioned );
        to.append("primary", _primary );
    }
    
    void DBConfig::unserialize(BSONObj& from){
        _name = from.getStringField("name");
        _partitioned = from.getBoolField("partitioned");
        _primary = from.getStringField("primary");
    }
    
    bool DBConfig::loadByName(const char *nm){
        BSONObjBuilder b;
        b.append("name", nm);
        BSONObj q = b.done();
        return load(q);
    }
    
    /* --- Grid --- */

    DBConfig* Grid::getDBConfig( string database ){
        {
            string::size_type i = database.find( "." );
            if ( i != string::npos )
                database = database.substr( 0 , i );
        }
        
        if ( database == "config" )
            return &configServer;

        DBConfig*& cc = _databases[database];
        if ( cc == 0 ) {
            cc = new DBConfig( database );
            if ( !cc->loadByName(database.c_str()) ) {
                // note here that cc->primary == 0.
                log() << "couldn't find database [" << database << "] in config db" << endl;
            }
        }
        
        return cc;
    }
    

    Grid grid;
} 
