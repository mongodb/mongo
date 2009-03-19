// whereExample.cpp

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

    const char * ns = "test.where";

    conn.remove( ns , BSONObj() );

    conn.insert( ns , BSON( "name" << "eliot" << "num" << 17 ) );
    conn.insert( ns , BSON( "name" << "sara" << "num" << 24 ) );
    
    auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObj() );
    
    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
    }

    cout << "now using $where" << endl;

    Query q = Query("{}").where("this.name == name" , BSON( "name" << "sara" ));

    cursor = conn.query( ns , q );

    int num = 0;
    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
        num++;
    }
    assert( num == 1 );
}
