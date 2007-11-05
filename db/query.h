// query.h

#pragma once

#include "../stdafx.h"
#include "../grid/message.h"
#include "jsobj.h"
#include "storage.h"

/* requests:

   dbDelete
      int reserved=0;
      string collection;
	  int flags=0; // 1=DeleteSingle
      JSObject query;
   dbUpdate:
      int reserved;
      string collection;
	  int flags; // 1=upsert
      JSObject query;
	  JSObject objectToUpdate;
   dbQuery:
      int reserved;
      string collection;
	  int nToReturn; // how many you want back as the beginning of the cursor data
      JSObject query;
   dbGetMore:
      int reserved;
	  string collection; // redundant, might use for security.
      int nToReturn;
      int64 cursorID;

   Note that on Update, there is only one object, which is different
   from insert where you can pass a list of objects to insert in the db.
   Note that the update field layout is very similar layout to Query.
*/

/* db response format

   Query or GetMore:
      int reserved;
      int64 cursorID;
      int startingFrom;
      int nReturned; // 0=infinity
      list of marshalled JSObjects;
*/

struct QueryResult : public MsgData {
	long long cursorId;
	int startingFrom;
	int nReturned;
	const char *data() { return (char *) (((int *)&nReturned)+1); }
};

QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid);
QueryResult* runQuery(const char *ns, int ntoreturn, JSObj);

void updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert);
void deleteObjects(const char *ns, JSObj pattern, bool justOne);

class Cursor;
class ClientCursor {
public:
	ClientCursor() { cursorid=0; pos=0; }
	~ClientCursor();
	long long cursorid;
	string ns;
	auto_ptr<JSMatcher> matcher;
	auto_ptr<Cursor> c;
	int pos;
	DiskLoc lastLoc;

	void updateLocation();
};

