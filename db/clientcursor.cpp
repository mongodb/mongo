// clientcursor.cpp

/* Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our client 
   application's perspective.
*/

#include "stdafx.h"
#include "query.h"
#include "introspect.h"
#include <time.h>

/* TODO: FIX cleanup of clientCursors when hit the end. */

typedef map<long long,ClientCursor*> CursorSet;
map<DiskLoc, CursorSet> byLocation;

CCMap clientCursors;

class CursInspector : public SingleResultObjCursor { 
	Cursor* clone() { return new CursInspector(*this); }
	void fill() { 
		b.append("byLocation_size", byLocation.size());
		b.append("clientCursors_size", clientCursors.size());

		stringstream ss;
		ss << '\n';
		int x = 40;
		map<DiskLoc,CursorSet>::iterator it = byLocation.begin();
		while( it != byLocation.end() ) {
			DiskLoc dl = it->first;
			CursorSet& v = it->second;
			ss << dl.toString() << " -> \n";

			for( CursorSet::iterator j = v.begin(); j != v.end(); j++ ) {
				ClientCursor *cc = j->second;
				ss << "    cid:" << cc->cursorid << ' ' << cc->ns << " pos:" << cc->pos << " LL:" << cc->lastLoc.toString();
				try { 
					setClient(cc->ns.c_str());
					Record *r = dl.rec();
					ss << " lwh:" << hex << r->lengthWithHeaders << " nxt:" << r->nextOfs << " prv:" << r->prevOfs << dec;
					if( r->nextOfs >= 0 && r->nextOfs < 16 ) 
						ss << " DELETED??? (!)";
				}
				catch(...) { 
					ss << " EXCEPTION";
				}
				ss << "\n";
			}
			if( --x <= 0 ) {
				ss << "only first 40 shown\n" << endl;
				break;
			}
			it++;
		}
		b.append("dump", ss.str().c_str());
	}
public:
	CursInspector() { reg("intr.cursors"); }
} _ciproto;

/* must call this when a btree node is updated */
void removedKey(const DiskLoc& btreeLoc, int keyPos) { 
// TODO!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
}

/* must call this on a delete so we clean up the cursors. */
void aboutToDelete(const DiskLoc& dl) { 
	map<DiskLoc,CursorSet>::iterator it = byLocation.find(dl);
	if( it != byLocation.end() ) {
		CursorSet& cset = it->second;

//		for( CursorSet::iterator j = cset.begin(); j != cset.end(); j++ ) {
		CursorSet::iterator j = cset.begin();
		while( j != cset.end() ) {
			ClientCursor *cc = j->second;
			assert( !cc->c->eof() );

			// check if we are done before calling updateLocation which might
			// delete cset
			j++;
			bool done = j == cset.end();

			// dm testing attempt
			cc->c->checkLocation();

			cc->c->advance();
			cc->updateLocation();

			if( done )
				break;
		}
	}
}

void ClientCursor::cleanupByLocation(DiskLoc loc, long long cursorid) { 
	if( !loc.isNull() ) { 
		map<DiskLoc, CursorSet>::iterator it = byLocation.find(loc);
		if( it != byLocation.end() ) {
			CursorSet& cset = it->second;
			int n = cset.erase(cursorid);
			wassert( n == 1 );
			if( cset.size() == 0 )
				byLocation.erase(it);
		}
	}
}

ClientCursor::~ClientCursor() {
	cleanupByLocation(lastLoc, cursorid);

	// defensive
	lastLoc.Null();
	cursorid = -1; 
	pos = -2;
}

// note this doesn't set lastLoc -- caller should.
void ClientCursor::addToByLocation(DiskLoc cl) { 
	byLocation[cl][cursorid] = this;
}

void ClientCursor::updateLocation() {

	DiskLoc cl = c->currLoc();
	//	cout<< "  TEMP: updateLocation last:" << lastLoc.toString() << " cl:" << cl.toString() << '\n';

	if( !lastLoc.isNull() )
		cleanupByLocation(lastLoc, cursorid);

	if( !cl.isNull() )
		addToByLocation(cl);

	lastLoc = cl;
	c->noteLocation();
}

/* report to us that a new clientcursor exists so we can track it. */
void ClientCursor::add(ClientCursor* cc) {
	clientCursors[cc->cursorid] = cc;
}

// todo: delete the ClientCursor.
// todo: other map
bool ClientCursor::erase(long long id) { 
	CCMap::iterator it = clientCursors.find(id);
	if( it != clientCursors.end() ) {
		ClientCursor *cc = it->second;
		clientCursors.erase(it);
		delete cc; // destructor will fix byLocation map
		return true;
	}
	return false;
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

