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
#include "mongo/client/dbclient.h"

#include <iostream>

#ifndef verify
#  define verify(x) MONGO_verify(x)
#endif

using namespace std;
using namespace mongo;

int main( int argc, const char **argv ) {

    const char *port = "27017";
    if ( argc != 1 ) {
        if ( argc != 3 ) {
            std::cout << "need to pass port as second param" << endl;
            return EXIT_FAILURE;
        }
        port = argv[ 2 ];
    }

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( string( "127.0.0.1:" ) + port , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        return EXIT_FAILURE;
    }

    const char * ns = "test.test1";

    conn.dropCollection(ns);

    // clean up old data from any previous tests
    conn.remove( ns, BSONObj() );
    verify( conn.findOne( ns , BSONObj() ).isEmpty() );

    // test insert
    conn.insert( ns ,BSON( "name" << "eliot" << "num" << 1 ) );
    verify( ! conn.findOne( ns , BSONObj() ).isEmpty() );

    // test remove
    conn.remove( ns, BSONObj() );
    verify( conn.findOne( ns , BSONObj() ).isEmpty() );


    // insert, findOne testing
    conn.insert( ns , BSON( "name" << "eliot" << "num" << 1 ) );
    {
        BSONObj res = conn.findOne( ns , BSONObj() );
        verify( strstr( res.getStringField( "name" ) , "eliot" ) );
        verify( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        verify( 1 == res.getIntField( "num" ) );
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
        verify( count == 2 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSON( "num" << 1 ) );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        verify( count == 1 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSON( "num" << 3 ) );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        verify( count == 0 );
    }

    // update
    {
        BSONObj res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() );
        verify( ! strstr( res.getStringField( "name2" ) , "eliot" ) );

        BSONObj after = BSONObjBuilder().appendElements( res ).append( "name2" , "h" ).obj();

        conn.update( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() , after );
        res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() );
        verify( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        verify( conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() ).isEmpty() );

        conn.update( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() , after );
        res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() );
        verify( strstr( res.getStringField( "name" ) , "eliot" ) );
        verify( strstr( res.getStringField( "name2" ) , "h" ) );
        verify( conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() ).isEmpty() );

        // upsert
        conn.update( ns , BSONObjBuilder().append( "name" , "eliot2" ).obj() , after , 1 );
        verify( ! conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).obj() ).isEmpty() );

    }

    {
        // ensure index
        verify( conn.ensureIndex( ns , BSON( "name" << 1 ) ) );
        verify( ! conn.ensureIndex( ns , BSON( "name" << 1 ) ) );
    }

    {
        // 5 second TTL index
        const char * ttlns = "test.ttltest1";
        conn.dropCollection( ttlns );

        {
            mongo::BSONObjBuilder b;
            b.appendTimeT("ttltime", time(0));
            b.append("name", "foo");
            conn.insert(ttlns, b.obj());
        }
        conn.ensureIndex(ttlns, BSON("ttltime" << 1), false, "", true, false, -1, 5);
        verify(!conn.findOne(ttlns, BSONObjBuilder().append("name", "foo").obj()).isEmpty());
        // Sleep 66 seconds, 60 seconds for the TTL loop, 5 seconds for the TTL and 1 to ensure
        sleepsecs(66);
        verify(conn.findOne(ttlns, BSONObjBuilder().append("name", "foo").obj()).isEmpty());
    }

    {
        // hint related tests
        verify( conn.findOne(ns, "{}")["name"].str() == "sara" );

        verify( conn.findOne(ns, "{ name : 'eliot' }")["name"].str() == "eliot" );
        verify( conn.getLastError() == "" );

        // nonexistent index test
        bool asserted = false;
        try {
            conn.findOne(ns, Query("{name:\"eliot\"}").hint("{foo:1}"));
        }
        catch ( ... ) {
            asserted = true;
        }
        verify( asserted );

        //existing index
        verify( conn.findOne(ns, Query("{name:'eliot'}").hint("{name:1}")).hasElement("name") );

        // run validate
        verify( conn.validate( ns ) );
    }

    {
        // timestamp test

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
        verify( ( oldTime < found["ts"].timestampTime() ) ||
                ( oldTime == found["ts"].timestampTime() && oldInc < found["ts"].timestampInc() ) );

    }

    {
        // check that killcursors doesn't affect last error
        verify( conn.getLastError().empty() );

        BufBuilder b;
        b.appendNum( (int)0 ); // reserved
        b.appendNum( (int)-1 ); // invalid # of cursors triggers exception
        b.appendNum( (int)-1 ); // bogus cursor id

        Message m;
        m.setData( dbKillCursors, b.buf(), b.len() );

        // say() is protected in DBClientConnection, so get superclass
        static_cast< DBConnector* >( &conn )->say( m );

        verify( conn.getLastError().empty() );
    }

    {
        list<string> l = conn.getDatabaseNames();
        for ( list<string>::iterator i = l.begin(); i != l.end(); i++ ) {
            cout << "db name : " << *i << endl;
        }

        l = conn.getCollectionNames( "test" );
        for ( list<string>::iterator i = l.begin(); i != l.end(); i++ ) {
            cout << "coll name : " << *i << endl;
        }
    }

    {
        //Map Reduce (this mostly just tests that it compiles with all output types)
        const string ns = "test.mr";
        conn.insert(ns, BSON("a" << 1));
        conn.insert(ns, BSON("a" << 1));

        const char* map = "function() { emit(this.a, 1); }";
        const char* reduce = "function(key, values) { return Array.sum(values); }";

        const string outcoll = ns + ".out";

        BSONObj out;
        out = conn.mapreduce(ns, map, reduce, BSONObj()); // default to inline
        //MONGO_PRINT(out);
        out = conn.mapreduce(ns, map, reduce, BSONObj(), outcoll);
        //MONGO_PRINT(out);
        out = conn.mapreduce(ns, map, reduce, BSONObj(), outcoll.c_str());
        //MONGO_PRINT(out);
        out = conn.mapreduce(ns, map, reduce, BSONObj(), BSON("reduce" << outcoll));
        //MONGO_PRINT(out);
    }

    { 
        // test timeouts

        DBClientConnection conn( true , 0 , 2 );
        if ( ! conn.connect( string( "127.0.0.1:" ) + port , errmsg ) ) {
            cout << "couldn't connect : " << errmsg << endl;
            throw -11;
        }
        conn.insert( "test.totest" , BSON( "x" << 1 ) );
        BSONObj res;
        
        bool gotError = false;
        verify( conn.eval( "test" , "return db.totest.findOne().x" , res ) );
        try {
            conn.eval( "test" , "sleep(5000); return db.totest.findOne().x" , res );
        }
        catch ( std::exception& e ) {
            gotError = true;
            log() << e.what() << endl;
        }
        verify( gotError );
        // sleep so the server isn't locked anymore
        sleepsecs( 4 );
        
        verify( conn.eval( "test" , "return db.totest.findOne().x" , res ) );
        
        
    }

    cout << "client test finished!" << endl;
    return EXIT_SUCCESS;
}
