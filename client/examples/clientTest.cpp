// clientTest.cpp

/**
 * a simple test for the c++ driver
 */

#include <iostream>

#include "client/dbclient.h"

using namespace std;
using namespace mongo;

int main() {

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( "127.0.0.1" , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    const char * ns = "test.test1";

    conn.dropCollection(ns);

    // clean up old data from any previous tests
    conn.remove( ns, BSONObjBuilder().doneAndDecouple() );
    assert( conn.findOne( ns , BSONObjBuilder().doneAndDecouple() ).isEmpty() );

    // test insert
    conn.insert( ns ,BSONObjBuilder().append( "name" , "eliot" ).append( "num" , 1 ).doneAndDecouple() );
    assert( ! conn.findOne( ns , BSONObjBuilder().doneAndDecouple() ).isEmpty() );

    // test remove
    conn.remove( ns, BSONObjBuilder().doneAndDecouple() );
    assert( conn.findOne( ns , BSONObjBuilder().doneAndDecouple() ).isEmpty() );


    // insert, findOne testing
    conn.insert( ns , BSONObjBuilder().append( "name" , "eliot" ).append( "num" , 1 ).doneAndDecouple() );
    {
        BSONObj res = conn.findOne( ns , BSONObjBuilder().doneAndDecouple() );
        assert( strstr( res.getStringField( "name" ) , "eliot" ) );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        assert( 1 == res.getIntField( "num" ) );
    }


    // cursor
    conn.insert( ns ,BSONObjBuilder().append( "name" , "sara" ).append( "num" , 2 ).doneAndDecouple() );
    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().doneAndDecouple() );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 2 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().append( "num" , 1 ).doneAndDecouple() );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 1 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().append( "num" , 3 ).doneAndDecouple() );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 0 );
    }

    // update
    {
        BSONObj res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).doneAndDecouple() );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );

        BSONObj after = BSONObjBuilder().appendElements( res ).append( "name2" , "h" ).doneAndDecouple();

        conn.update( ns , BSONObjBuilder().append( "name" , "eliot2" ).doneAndDecouple() , after );
        res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).doneAndDecouple() );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        assert( conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot2" ).doneAndDecouple() ).isEmpty() );

        conn.update( ns , BSONObjBuilder().append( "name" , "eliot" ).doneAndDecouple() , after );
        res = conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).doneAndDecouple() );
        assert( strstr( res.getStringField( "name" ) , "eliot" ) );
        assert( strstr( res.getStringField( "name2" ) , "h" ) );
        assert( conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot2" ).doneAndDecouple() ).isEmpty() );

        // upsert
        conn.update( ns , BSONObjBuilder().append( "name" , "eliot2" ).doneAndDecouple() , after , 1 );
        assert( ! conn.findOne( ns , BSONObjBuilder().append( "name" , "eliot" ).doneAndDecouple() ).isEmpty() );

    }

    { // ensure index
        assert( conn.ensureIndex( ns , BSON( "name" << 1 ) ) );
        assert( ! conn.ensureIndex( ns , BSON( "name" << 1 ) ) );
    }

    { // hint related tests
        assert( conn.findOne(ns, "{}")["name"].str() == "sara" );

        assert( conn.findOne(ns, "{name:\"eliot\"}")["name"].str() == "eliot" );

        // nonexistent index test
        assert( conn.findOne(ns, Query("{name:\"eliot\"}").hint("{foo:1}")).hasElement("$err") );

        //existing index
        assert( conn.findOne(ns, Query("{name:\"eliot\"}").hint("{name:1}")).hasElement("name") );
    }

    cout << "client test finished!" << endl;
}
