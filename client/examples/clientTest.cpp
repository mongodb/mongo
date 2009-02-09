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
    if ( ! conn.connect(/* "192.168.58.1"*/"127.0.0.1" , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    const char * ns = "test.test1";

    conn.dropCollection(ns);

    // clean up old data from any previous tests
    conn.remove( ns, BSONObjBuilder().obj() );
    assert( conn.findOne( ns , BSONObjBuilder().obj() ).isEmpty() );

    // test insert
    conn.insert( ns ,BSONObjBuilder().append( "name" , "eliot" ).append( "num" , 1 ).obj() );
    assert( ! conn.findOne( ns , BSONObjBuilder().obj() ).isEmpty() );

    // test remove
    conn.remove( ns, BSONObjBuilder().obj() );
    assert( conn.findOne( ns , BSONObjBuilder().obj() ).isEmpty() );


    // insert, findOne testing
    conn.insert( ns , BSONObjBuilder().append( "name" , "eliot" ).append( "num" , 1 ).obj() );
    {
        BSONObj res = conn.findOne( ns , BSONObjBuilder().obj() );
        assert( strstr( res.getStringField( "name" ) , "eliot" ) );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        assert( 1 == res.getIntField( "num" ) );
    }


    // cursor
    conn.insert( ns ,BSONObjBuilder().append( "name" , "sara" ).append( "num" , 2 ).obj() );
    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().obj() );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 2 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().append( "num" , 1 ).obj() );
        int count = 0;
        while ( cursor->more() ) {
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 1 );
    }

    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().append( "num" , 3 ).obj() );
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
        assert( conn.findOne(ns, Query("{name:\"eliot\"}").hint("{foo:1}")).hasElement("$err") );
        assert( conn.getLastError() == "hint index not found" );
        conn.resetError();
        assert( conn.getLastError() == "" );

        //existing index
        assert( conn.findOne(ns, Query("{name:'eliot'}").hint("{name:1}")).hasElement("name") );

        // run validate
        assert( conn.validate( ns ) );
    }
    
    cout << "client test finished!" << endl;
}
