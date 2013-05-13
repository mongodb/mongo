// tail.cpp

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

/* example of using a tailable cursor */

#include "mongo/client/dbclient.h"
#include "mongo/util/goodies.h"

using namespace mongo;

void tail(DBClientBase& conn, const char *ns) {
    BSONElement lastId = minKey.firstElement();
    Query query = Query();

    auto_ptr<DBClientCursor> c =
        conn.query(ns, query, 0, 0, 0, QueryOption_CursorTailable);

    while( 1 ) {
        if( !c->more() ) {
            if( c->isDead() ) {
                break;    // we need to requery
            }

            // all data (so far) exhausted, wait for more
            sleepsecs(1);
            continue;
        }
        BSONObj o = c->next();
        lastId = o["_id"];
        cout << o.toString() << endl;
    }
}
