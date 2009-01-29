// whereExample.cpp

#include <iostream>

#include "mongo/client/dbclient.h"

using namespace std;
using namespace mongo;

int main() {

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( "127.0.0.1" , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    const char * ns = "test.where";

    conn.remove( ns , emptyObj );

    conn.insert( ns , BUILDOBJ( "name" << "eliot" << "num" << 17 ) );
    conn.insert( ns , BUILDOBJ( "name" << "sara" << "num" << 24 ) );
    
    auto_ptr<DBClientCursor> cursor = conn.query( ns , emptyObj );
    
    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
    }

    cout << "now using $where" << endl;

    BSONObjBuilder query;

    query.appendWhere( "this.name == name" , BUILDOBJ( "name" << "sara" ) );

    cursor = conn.query( ns , query.done() );

    int num = 0;
    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
        num++;
    }
    assert( num == 1 );
}
