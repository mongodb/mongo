// @file whereExample.cpp
// @see http://dochub.mongodb.org/core/serversidecodeexecution

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

#include <iostream>

#include "mongo/client/dbclient.h"

#ifndef verify
#  define verify(x) MONGO_verify(x)
#endif

using namespace std;
using namespace mongo;

int main( int argc, const char **argv ) {

    const char *port = "27017";
    if ( argc != 1 ) {
        if ( argc != 3 ) {
            cout << "need to pass port as second param" << endl;
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

    const char * ns = "test.where";

    conn.remove( ns , BSONObj() );

    conn.insert( ns , BSON( "name" << "eliot" << "num" << 17 ) );
    conn.insert( ns , BSON( "name" << "sara" << "num" << 24 ) );

    std::auto_ptr<DBClientCursor> cursor = conn.query( ns , BSONObj() );
    if (!cursor.get()) {
        cout << "query failure" << endl;
        return EXIT_FAILURE;
    }

    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
    }

    cout << "now using $where" << endl;

    Query q = Query("{}").where("this.name == name" , BSON( "name" << "sara" ));

    cursor = conn.query( ns , q );
    if (!cursor.get()) {
        cout << "query failure" << endl;
        return EXIT_FAILURE;
    }

    int num = 0;
    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
        num++;
    }
    verify( num == 1 );

    return EXIT_SUCCESS;
}
