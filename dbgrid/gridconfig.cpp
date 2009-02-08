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

    /* --- Machine --- */

    map<string, Machine*> Machine::machines;

    /* --- DBConfig --- */

    bool DBConfig::partitioned( const NamespaceString& ns ){
        if ( ! _partitioned )
            return false;
        uassert( "don't know what to do!" , 0 );
        return 0;
    }

    Machine * DBConfig::getMachine( const NamespaceString& ns ){
        if ( partitioned( ns ) )
            return 0;
        
        uassert( "no primary!" , _primary );
        return _primary;
    }

    void DBConfig::serialize(BSONObjBuilder& to){
        to.append("name", _name);
        to.appendBool("partitioned", _partitioned );
        if ( _primary )
            to.append("primary", _primary->getName() );
    }
    
    void DBConfig::unserialize(BSONObj& from){
        _name = from.getStringField("name");
        _partitioned = from.getBoolField("partitioned");
        string p = from.getStringField("primary");
        if ( ! p.empty() )
            _primary = Machine::get(p);
    }
    
    bool DBConfig::loadByName(const char *nm){
        BSONObjBuilder b;
        b.append("name", nm);
        BSONObj q = b.done();
        return load(q);
    }
    
    DBClientWithCommands* GridConfigModel::conn(){
        return configServer.conn();
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
            cc = new DBConfig();
            if ( !cc->loadByName(database.c_str()) ) {
                // note here that cc->primary == 0.
                log() << "couldn't find database [" << database << "] in config db" << endl;
            }
        }
        
        return cc;
    }
    

    Grid grid;
} 
