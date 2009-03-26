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
        ScopedDbConnection conn( modelServer() );

        BSONObj b = conn->findOne(getNS(), query);
        conn.done();
        
        if ( b.isEmpty() )
            return false;
        
        unserialize(b);
        _id = b["_id"].wrap().getOwned();
        return true;
    }

    void Model::save( bool check ){
        ScopedDbConnection conn( modelServer() );

        BSONObjBuilder b;
        serialize( b );
        
        if ( _id.isEmpty() ){
            OID oid;
            oid.init();
            b.appendOID( "_id" , &oid );
            
            BSONObj o = b.obj();
            conn->insert( getNS() , o );
            _id = o["_id"].wrap().getOwned();

            log(4) << "inserted new model " << getNS() << "  " << o << endl;
        }
        else {
            BSONElement id = _id["_id"];
            b.append( id );

            BSONObjBuilder qb;
            qb.append( id );
            
            BSONObj q = qb.obj();
            BSONObj o = b.obj();

            log(4) << "updated old model" << getNS() << "  " << q << " " << o << endl;

            conn->update( getNS() , q , o );
            
        }
        
        string errmsg = "";
        if ( check )
            errmsg = conn->getLastError();

        conn.done();

        if ( check && errmsg.size() )
            throw UserException( (string)"error on Model::save: " + errmsg );
    }

} // namespace mongo
