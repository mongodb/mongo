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
        objectToUpdate may include { $inc: <field> }.
   dbQuery:
      int reserved;
      string collection;
	  int nToSkip;
	  int nToReturn; // how many you want back as the beginning of the cursor data
      JSObject query;
	  [JSObject fieldsToReturn]
   dbGetMore:
      int reserved;
	  string collection; // redundant, might use for security.
      int nToReturn;
      int64 cursorID;
   dbKillCursors=2007
      int reserved;
      int n;
	  int64 cursorIDs[n];

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

#pragma pack(push)
#pragma pack(1)

struct QueryResult : public MsgData {
	long long cursorId;
	int startingFrom;
	int nReturned;
	const char *data() { return (char *) (((int *)&nReturned)+1); }
};

#pragma pack(pop)

QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid);

// caller must free() returned QueryResult.
QueryResult* runQuery(const char *ns, int ntoskip, int ntoreturn, 
					  JSObj j, auto_ptr< set<string> > fieldFilter,
					  stringstream&);

void updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert);
void deleteObjects(const char *ns, JSObj pattern, bool justOne);

class ClientCursor;
typedef map<long long, ClientCursor*> CCMap;
extern CCMap clientCursors; /* cursorid -> ClientCursor */

/* Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our client 
   application's perspective.
*/
class Cursor;
class ClientCursor {
	friend class CursInspector;
public:
	ClientCursor() { cursorid=0; pos=0; nextAtThisLocation=0; }
	~ClientCursor();
	long long cursorid;
	string ns;
	auto_ptr<JSMatcher> matcher;
	auto_ptr<Cursor> c;
	int pos;
	DiskLoc lastLoc;
	auto_ptr< set<string> > filter;

	/* report to us that a new clientcursor exists so we can track it. You still
	   do the initial updateLocation() yourself. 
	   */
	static void add(ClientCursor*);

	static bool erase(long long cursorid);

	static ClientCursor* find(long long id) {
		CCMap::iterator it = clientCursors.find(id);
		if( it == clientCursors.end() ) { 
			cout << "ClientCursor::find(): cursor not found in map " << id << endl;
			return 0;
		}
		return it->second;
	}

	/* call when cursor's location changes so that we can update the 
	   cursorsbylocation map.  if you are locked and internally iterating, only 
	   need to call when you are ready to "unlock".
	   */
	void updateLocation();

private:
	void addToByLocation(DiskLoc cl);
	static void cleanupByLocation(DiskLoc loc, long long cursorid);
public:
	ClientCursor *nextAtThisLocation;
};

long long allocCursorId();
