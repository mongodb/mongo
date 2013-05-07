// second.cpp

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

    ScopedDbConnection conn(string( "127.0.0.1:" ) + port);

    const char * ns = "test.second";

    conn->remove( ns , BSONObj() );

    conn->insert( ns , BSON( "name" << "eliot" << "num" << 17 ) );
    conn->insert( ns , BSON( "name" << "sara" << "num" << 24 ) );

    std::auto_ptr<DBClientCursor> cursor = conn->query( ns , BSONObj() );

    if (!cursor.get()) {
        cout << "query failure" << endl;
        return EXIT_FAILURE;
    }

    cout << "using cursor" << endl;
    while ( cursor->more() ) {
        BSONObj obj = cursor->next();
        cout << "\t" << obj.jsonString() << endl;
    }

    conn->ensureIndex( ns , BSON( "name" << 1 << "num" << -1 ) );

    conn.done();

    return EXIT_SUCCESS;
}
