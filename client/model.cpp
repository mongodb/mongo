// model.cpp

/*    Copyright 2009 10gen
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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

    void Model::remove( bool safe ){
        uassert( "_id isn't set - needed for remove()" , _id["_id"].type() );
        
        ScopedDbConnection conn( modelServer() );
        conn->remove( getNS() , _id );

        string errmsg = "";
        if ( safe )
            errmsg = conn->getLastError();

        conn.done();
        
        if ( safe && errmsg.size() )
            throw UserException( (string)"error on Model::remove: " + errmsg );
    }

    void Model::save( bool safe ){
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
        if ( safe )
            errmsg = conn->getLastError();

        conn.done();

        if ( safe && errmsg.size() )
            throw UserException( (string)"error on Model::save: " + errmsg );
    }

} // namespace mongo
