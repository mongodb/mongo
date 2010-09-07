// clientTest.cpp

/*    Copyright 2009 10gen Inc.
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

/**
 * a simple test for the c++ driver
 */

// this header should be first to ensure that it includes cleanly in any context
#include "client/dbclient.h"

#include <iostream>

#ifndef assert
#  define assert(x) MONGO_assert(x)
#endif

using namespace std;
using namespace mongo;

int main( int argc, const char **argv ) {

    const char *port = "27017";
    if ( argc != 1 ) {
        if ( argc != 3 )
            throw -12;
        port = argv[ 2 ];
    }

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( string( "127.0.0.1:" ) + port , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    const char * ns = "test.test1";

    conn.dropCollection(ns);

    // clean up old data from any previous tests
    conn.remove( ns, BSONObj() );
    assert( conn.findOne( ns , BSONObj() ).isEmpty() );

    // test insert
    conn.insert( ns ,BSON( "name" << "eliot" << "num" << 1 ) );
    assert( ! conn.findOne( ns , BSONObj() ).isEmpty() );

    // test remove
    conn.remove( ns, BSONObj() );
    assert( conn.findOne( ns , BSONObj() ).isEmpty() );


    // insert, findOne testing
    conn.insert( ns , BSON( "name" << "eliot" << "num" << 1 ) );
    {
        BSONObj res = conn.findOne( ns , BSONObj() );
        assert( strstr( res.getStringField( "name" ) , "eliot" ) );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        assert( 1 == res.getIntField( "num" ) );
    }


    // cursor
    conn.insert( ns ,BSON( "name" << "sara" << "num" << 2 ) );
    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObj() );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 2 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSON( "num" << 1 ) );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 1 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSON( "num" << 3 ) );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 0 );
    }

    // update
    {
        BSONObj res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );

        BSONObj after = BSONObjBuilder().appendElements( res ).append( "name2" , "h" ).obj();

        conn.update( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() , after );
        res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        assert( conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() ).isEmpty() );

        conn.update( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() , after );
        res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() );
        assert( strstr( res.getStringField( "name" ) , "eliot" ) );
        assert( strstr( res.getStringField( "name2" ) , "h" ) );
        assert( conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() ).isEmpty() );

        // upsert
        conn.update( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() , after , 1 );
        assert( ! conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() ).isEmpty() );

    }

    { // ensure index
        assert( conn.ensureIndex( ns , BSON( "name" << 1 ) ) );
        assert( ! conn.ensureIndex( ns , BSON( "name" << 1 ) ) );
    }

    { // hint related tests
        assert( conn.findOne(ns, "{}")["name"].str() == "sara" );

        assert( conn.findOne(ns, "{ name : 'eliot' }")["name"].str() == "eliot" );
        assert( conn.getLastError() == "" );

        // nonexistent index test
        bool asserted = false;
        try {
            conn.findOne(ns, Query("{name:\"eliot\"}").hint("{foo:1}"));
        }
        catch ( ... ){
            asserted = true;
        }
        assert( asserted );

        //existing index
        assert( conn.findOne(ns, Query("{name:'eliot'}").hint("{name:1}")).hasElement("name") );

        // run validate
        assert( conn.validate( ns ) );
    }

    { // timestamp test

        const char * tsns = "test.tstest1";
        conn.dropCollection( tsns );

        {
            mongo::BSONObjBuilder b;
            b.appendTimestamp( "ts" );
            conn.insert( tsns , b.obj() );
        }

        mongo::BSONObj out = conn.findOne( tsns , mongo::BSONObj() );
        Date_t oldTime = out["ts"].timestampTime();
        unsigned int oldInc = out["ts"].timestampInc();

        {
            mongo::BSONObjBuilder b1;
            b1.append( out["_id"] );

            mongo::BSONObjBuilder b2;
            b2.append( out["_id"] );
            b2.appendTimestamp( "ts" );

            conn.update( tsns , b1.obj() , b2.obj() );
        }

        BSONObj found = conn.findOne( tsns , mongo::BSONObj() );
        cout << "old: " << out << "\nnew: " << found << endl;
        assert( ( oldTime < found["ts"].timestampTime() ) ||
                ( oldTime == found["ts"].timestampTime() && oldInc < found["ts"].timestampInc() ) );

    }
    
    { // check that killcursors doesn't affect last error
        assert( conn.getLastError().empty() );
        
        BufBuilder b;
        b.appendNum( (int)0 ); // reserved
        b.appendNum( (int)-1 ); // invalid # of cursors triggers exception
        b.appendNum( (int)-1 ); // bogus cursor id
        
        Message m;
        m.setData( dbKillCursors, b.buf(), b.len() );
        
        // say() is protected in DBClientConnection, so get superclass
        static_cast< DBConnector* >( &conn )->say( m );
        
        assert( conn.getLastError().empty() );
    }

    {
        list<string> l = conn.getDatabaseNames();
        for ( list<string>::iterator i = l.begin(); i != l.end(); i++ ){
            cout << "db name : " << *i << endl;
        }

        l = conn.getCollectionNames( "test" );
        for ( list<string>::iterator i = l.begin(); i != l.end(); i++ ){
            cout << "coll name : " << *i << endl;
        }
    }

    cout << "client test finished!" << endl;
}
