// authTest.cpp

#include <iostream>

#include "client/dbclient.h"

using namespace mongo;

int main() {

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( "127.0.0.1" , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    { // clean up old data from any previous tests
        conn.remove( "test.system.users" , BSONObj() );
    }

    conn.insert( "test.system.users" , BSON( "user" << "eliot" << "pwd" << conn.createPasswordDigest( "eliot" , "bar" ) ) );
    
    errmsg.clear();
    bool ok = conn.auth( "test" , "eliot" , "bar" , errmsg );
    if ( ! ok )
        cout << errmsg << endl;
    assert( ok );

    assert( ! conn.auth( "test" , "eliot" , "bars" , errmsg ) );
}
