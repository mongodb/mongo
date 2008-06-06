/* clientcursor.h

   Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our client 
   application's perspective.
*/

#pragma once

#include "../stdafx.h"

typedef long long CursorId;
class Cursor;
class ClientCursor;
typedef map<CursorId, ClientCursor*> CCById;
extern CCById clientCursorsById;

class ClientCursor {
	friend class CursInspector;
public:
	ClientCursor() { 
		cursorid=0; pos=0; 
	}
	~ClientCursor();
	CursorId cursorid;
	string ns;
	auto_ptr<JSMatcher> matcher;
	auto_ptr<Cursor> c;
	int pos;
	DiskLoc lastLoc;
	auto_ptr< set<string> > filter; // which fields query wants returned

	/* report to us that a new clientcursor exists so we can track it.
	   note you do not need to call updateLocation, but location should be set before
	   calling.
	   */
	static void add(ClientCursor*);

	static bool erase(CursorId cursorid);

	static ClientCursor* find(CursorId id) {
		CCById::iterator it = clientCursorsById.find(id);
		if( it == clientCursorsById.end() ) { 
			cout << "ClientCursor::find(): cursor not found in map " << id << '\n';
			return 0;
		}
		return it->second;
	}

	/* call when cursor's location changes so that we can update the 
	   cursorsbylocation map.  if you are locked and internally iterating, only 
	   need to call when you are ready to "unlock".
	   */
	void updateLocation();

//private:
//	void addToByLocation(DiskLoc cl);
	void cleanupByLocation(DiskLoc loc);
//public:
//	ClientCursor *nextAtThisLocation;
};

CursorId allocCursorId();
