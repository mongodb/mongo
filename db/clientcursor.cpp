// clientcursor.cpp

/* Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our client 
   application's perspective.
*/

#include "stdafx.h"
#include "query.h"
#include "introspect.h"
#include <time.h>

map<DiskLoc, ClientCursor*> cursorsByLocation;
CCMap clientCursors;

class CursInspector : public SingleResultObjCursor { 
	Cursor* clone() { return new CursInspector(*this); }
	void fill() { 
		b.append("cursorsByLocation", cursorsByLocation.size());
		b.append("clientCursors", clientCursors.size());
	}
public:
	CursInspector() { reg("intr.cursors"); }
} _ciproto;

/* must call this when a btree node is updated */
void removedKey(const DiskLoc& btreeLoc, int keyPos) { 
}

/* must call this on a delete so we clean up the cursors. */
void aboutToDelete(const DiskLoc& dl) { 
	map<DiskLoc,ClientCursor*>::iterator it = cursorsByLocation.find(dl);
	if( it != cursorsByLocation.end() ) {
		ClientCursor *cc = it->second;
		assert( !cc->c->eof() );

// dm testing attempt
cc->c->checkLocation();

		cc->c->advance();
		cc->updateLocation();
	}
}

ClientCursor::~ClientCursor() {
	if( !lastLoc.isNull() ) { 
		int n = cursorsByLocation.erase(lastLoc);
		wassert( n == 1 );
	}
	lastLoc.Null();
}

void ClientCursor::updateLocation() {

	DiskLoc cl = c->currLoc();
//	cout<< "  TEMP: updateLocation last:" << lastLoc.toString() << " cl:" << cl.toString() << '\n';

	if( !lastLoc.isNull() ) { 
		int n = cursorsByLocation.erase(lastLoc);
		assert( n == 1 );
	}
	if( !cl.isNull() )
		cursorsByLocation[cl] = this;
	lastLoc = cl;
	c->noteLocation();
}

/* report to us that a new clientcursor exists so we can track it. */
void ClientCursor::add(ClientCursor* cc) {
	clientCursors[cc->cursorid] = cc;
}

long long allocCursorId() { 
	long long x;
	while( 1 ) {
		x = (((long long)rand()) << 32);
		x = x | time(0);
		if( clientCursors.count(x) == 0 )
			break;
	}
	return x;
}

