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
