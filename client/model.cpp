// model.cpp

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
#include "model.h"
#include "connpool.h"

namespace mongo {

    bool Model::load(BSONObj& query){
        ScopedDbConnection scoped( modelServer() );
        DBClientWithCommands& conn = scoped.conn();
        BSONObj b = conn.findOne(getNS(), query);
        scoped.done();
        
        if ( b.isEmpty() )
            return false;
        
        unserialize(b);
        _id = b["_id"];
        return true;
    }

    void Model::save(){
        ScopedDbConnection scoped( modelServer() );
        DBClientWithCommands& conn = scoped.conn();

        BSONObjBuilder b;
        serialize( b );
        
        if ( _id.eoo() ){
            OID oid;
            b.appendOID( "_id" , &oid );
            
            BSONObj o = b.obj();
            conn.insert( getNS() , o );
            _id = o["_id"];

            log(4) << "inserted new model" << endl;
        }
        else {
            b.append( _id );
            BSONObjBuilder id;
            id.append( _id );
            conn.update( getNS() , id.obj() , b.obj() );
            
            log(4) << "updated old model" << endl;
        }

        scoped.done();
    }

} // namespace mongo
