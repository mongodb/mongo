// clientTest.cpp

/**
 * a simple test for the c++ driver
 */

#include <iostream>

#include "mongo/client/dbclient.h"

using namespace std;

int main(){

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( "127.0.0.1" , errmsg ) ){
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }
    
    const char * ns = "test.test1";

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
    conn.insert( ns ,BSONObjBuilder().append( "name" , "eliot" ).append( "num" , 1 ).doneAndDecouple() );    
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
        while ( cursor->more() ){    
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 2 );
    }
    
    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().append( "num" , 1 ).doneAndDecouple() );
        int count = 0;
        while ( cursor->more() ){    
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 1 );
    }
    
    {
        auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObjBuilder().append( "num" , 3 ).doneAndDecouple() );
        int count = 0;
        while ( cursor->more() ){    
            count++;
            BSONObj obj = cursor->next();
        }
        assert( count == 0 );
    }

    cout << "client test finished!" << endl;
}
