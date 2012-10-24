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

#include "pch.h"
#include "model.h"
#include "connpool.h"

namespace mongo {

    bool Model::load(BSONObj& query) {
        scoped_ptr<ScopedDbConnection> conn(
                ScopedDbConnection::getScopedDbConnection (modelServer() ) );

        BSONObj b = conn->get()->findOne(getNS(), query);
        conn->done();

        if ( b.isEmpty() )
            return false;

        unserialize(b);
        _id = b["_id"].wrap().getOwned();
        return true;
    }

    void Model::remove( bool safe ) {
        uassert( 10016 ,  "_id isn't set - needed for remove()" , _id["_id"].type() );

        scoped_ptr<ScopedDbConnection> conn(
                ScopedDbConnection::getScopedDbConnection (modelServer() ) );
        conn->get()->remove( getNS() , _id );

        string errmsg = "";
        if ( safe )
            errmsg = conn->get()->getLastError();

        conn->done();

        if ( safe && errmsg.size() )
            throw UserException( 9002 , (string)"error on Model::remove: " + errmsg );
    }

    void Model::save( bool safe ) {
        scoped_ptr<ScopedDbConnection> conn(
                ScopedDbConnection::getScopedDbConnection (modelServer() ) );

        BSONObjBuilder b;
        serialize( b );

        BSONElement myId;
        {
            BSONObjIterator i = b.iterator();
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( strcmp( e.fieldName() , "_id" ) == 0 ) {
                    myId = e;
                    break;
                }
            }
        }

        if ( myId.type() ) {
            if ( _id.isEmpty() ) {
                _id = myId.wrap();
            }
            else if ( myId.woCompare( _id.firstElement() ) ) {
                stringstream ss;
                ss << "_id from serialize and stored differ: ";
                ss << '[' << myId << "] != ";
                ss << '[' << _id.firstElement() << ']';
                throw UserException( 13121 , ss.str() );
            }
        }

        if ( _id.isEmpty() ) {
            OID oid;
            oid.init();
            b.appendOID( "_id" , &oid );

            BSONObj o = b.obj();
            conn->get()->insert( getNS() , o );
            _id = o["_id"].wrap().getOwned();

            LOG(4) << "inserted new model " << getNS() << "  " << o << endl;
        }
        else {
            if ( myId.eoo() ) {
                myId = _id["_id"];
                b.append( myId );
            }

            verify( ! myId.eoo() );

            BSONObjBuilder qb;
            qb.append( myId );

            BSONObj q = qb.obj();
            BSONObj o = b.obj();

            LOG(4) << "updated model" << getNS() << "  " << q << " " << o << endl;

            conn->get()->update( getNS() , q , o , true );

        }

        string errmsg = "";
        if ( safe )
            errmsg = conn->get()->getLastError();

        conn->done();

        if ( safe && errmsg.size() )
            throw UserException( 9003 , (string)"error on Model::save: " + errmsg );
    }

    BSONObj Model::toObject() {
        BSONObjBuilder b;
        serialize( b );
        return b.obj();
    }

    void Model::append( const char * name , BSONObjBuilder& b ) {
        BSONObjBuilder bb( b.subobjStart( name ) );
        serialize( bb );
        bb.done();
    }

} // namespace mongo
