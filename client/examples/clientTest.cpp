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
    conn.insert( ns ,BSON( "name" << "eliot" << "num" << 1 ) );
    assert( ! conn.findOne( ns , BSONObjBuilder().obj() ).isEmpty() );

    // test remove
    conn.remove( ns, BSONObjBuilder().obj() );
    assert( conn.findOne( ns , BSONObjBuilder().obj() ).isEmpty() );


    // insert, findOne testing
    conn.insert( ns , BSON( "name" << "eliot" << "num" << 1 ) );
    {
        BSONObj res = conn.findOne( ns , BSONObjBuilder().obj() );
        assert( strstr( res.getStringField( "name" ) , "eliot" ) );
        assert( ! strstr( res.getStringField( "name2" ) , "eliot" ) );
        assert( 1 == res.getIntField( "num" ) );
    }


    // cursor
    conn.insert( ns ,BSON( "name" << "sara" << "num" << 2 ) );
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
        assert( conn.findOne(ns, Query("{name:\"eliot\"}").hint("{foo:1}")).hasElement("$err") );
        assert( conn.getLastError() == "bad hint" );
        conn.resetError();
        assert( conn.getLastError() == "" );

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
        unsigned long long oldTime = out["ts"].timestampTime();
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
        assert( ( oldTime < found["ts"].timestampTime() ) ||
               ( oldInc + 1 == found["ts"].timestampInc() ) );
        
    }
    
    cout << "client test finished!" << endl;
}
