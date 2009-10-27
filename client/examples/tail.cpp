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

#include "../../client/dbclient.h"
#include "../../util/goodies.h"

using namespace mongo;

void foo() { }

/* "tail" the specified namespace, outputting elements as they are added. 
   _id values must be inserted in increasing order for this to work. (Some other 
   field could also be used.)

   Note: one could use a capped collection and $natural order to do something 
         similar, using sort({$natural:1}), and then not need to worry about 
	 _id's being in order.
*/
void tail(DBClientBase& conn, const char *ns) {
  conn.ensureIndex(ns, fromjson("{_id:1}"));
  BSONElement lastId;
  Query query = Query().sort("_id");
  while( 1 ) {
    auto_ptr<DBClientCursor> c = conn.query(ns, query, 0, 0, 0, Option_CursorTailable);
    while( 1 ) {
      if( !c->more() ) { 
	if( c->isDead() ) {
	  // we need to requery
	  break;
	}
	sleepsecs(1);
      }
      BSONObj o = c->next();
      lastId = o["_id"];
      cout << o.toString() << endl;
    }
    query = QUERY( "_id" << GT << lastId ).sort("_id");
  }
}
