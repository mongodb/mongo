// second.cpp

#include <iostream>

#include "mongo/client/dbclient.h"

using namespace std;
using namespace mongo;

int main(){

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( "127.0.0.1" , errmsg ) ){
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    const char * ns = "test.second";

    conn.remove( ns , emptyObj );
    
    conn.insert( ns , BUILDOBJ( "name" << "eliot" << "num" << 17 ) );
    conn.insert( ns , BUILDOBJ( "name" << "sara" << "num" << 24 ) );

    auto_ptr<DBClientCursor> cursor = conn.query( ns , emptyObj );
    cout << "using cursor" << endl;
    while ( cursor->more() ){    
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
    }

    conn.ensureIndex( ns , BUILDOBJ( "name" << 1 << "num" << -1 ) );
}
