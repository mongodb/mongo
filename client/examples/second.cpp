// second.cpp

#include <iostream>

#include "client/dbclient.h"

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

    const char * ns = "test.second";

    conn.remove( ns , BSONObj() );

    conn.insert( ns , BSON( "name" << "eliot" << "num" << 17 ) );
    conn.insert( ns , BSON( "name" << "sara" << "num" << 24 ) );

    auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObj() );
    cout << "using cursor" << endl;
    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
    }

    conn.ensureIndex( ns , BSON( "name" << 1 << "num" << -1 ) );
}
