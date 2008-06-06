// clientcursor.cpp

/* Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our client 
   application's perspective.
*/

#include "stdafx.h"
#include "query.h"
#include "introspect.h"
#include <time.h>

/* TODO: FIX cleanup of clientCursors when hit the end. (ntoreturn insufficient) */

typedef map<DiskLoc, set<ClientCursor*>> DiskLocToCC;
DiskLocToCC clientCursorsByLocation;

CCById clientCursorsById;

/* must call this when a btree node is updated */
void removedKey(const DiskLoc& btreeLoc, int keyPos) { 
// TODO!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
}

/* must call this on a delete so we clean up the cursors. */
void aboutToDelete(const DiskLoc& dl) { 
	DiskLocToCC::iterator it = clientCursorsByLocation.find(dl);

	if( it != clientCursorsByLocation.end() ) {
		set<ClientCursor*>& ccs = it->second;

		set<ClientCursor*>::iterator it = ccs.begin();
		while( it != ccs.end() ) { 
			ClientCursor *cc = *it;
			cc->c->checkLocation();
			cc->c->advance();
			assert( cc->currLoc() != dl ); // assert that we actually advanced
			cc->lastLoc.Null(); //  updateLocation must not try to remove, we are cleaining up this list ourself.
			cc->updateLocation();
		}

		clientCursorsByLocation.erase(it);
	}
}

void ClientCursor::cleanupByLocation(DiskLoc loc) { 
	if( loc.isNull() )
		return;

	DiskToCC::iterator it = byLocation.find(loc);
	if( it != byLocation.end() ) {
		it->second.erase(this);
		if( it->second.empty() )
			it->erase();
	}
}

ClientCursor::~ClientCursor() {
#if defined(_WIN32)
	cout << "~clientcursor " << cursorid << endl;
#endif
	assert( pos != -2 );
	cleanupByLocation(lastLoc);
	assert( pos != -2 );

	// defensive
	lastLoc.Null();
	cursorid = -1; 
	pos = -2;
	nextAtThisLocation = 0;
}

// note this doesn't set lastLoc -- caller should.
void ClientCursor::addToByLocation(DiskLoc cl) { 
	assert( cursorid );
	clientCursorsByLocation[cl].insert(this);
}

/* call when cursor's location changes so that we can update the 
   cursorsbylocation map.  if you are locked and internally iterating, only 
   need to call when you are ready to "unlock".
*/
void ClientCursor::updateLocation() {
	assert( cursorid );
	DiskLoc cl = c->currLoc();

	if( lastLoc == cl ) {
		cout << "info: lastloc==curloc " << ns << '\n';
		return;
	}

	if( !lastLoc.isNull() )
		cleanupByLocation(lastLoc, cursorid);

	if( !cl.isNull() )
		addToByLocation(cl);

	lastLoc = cl;
	c->noteLocation();
}

/* report to us that a new clientcursor exists so we can track it. 
   note you still must call updateLocation (which likely should be changed)
*/
void ClientCursor::add(ClientCursor* cc) {
	clientCursors[cc->cursorid] = cc;
	updateLocation();
}

bool ClientCursor::erase(long long id) { 
	CCById::iterator it = clientCursorsById.find(id);
	if( it != clientCursorsById.end() ) {
		ClientCursor *cc = it->second;
		it->second = 0; // defensive
		clientCursorsById.erase(it);
		delete cc; // destructor will fix byLocation map
		return true;
	}
	return false;
}

long long allocCursorId() { 
	long long x;
	while( 1 ) {
		x = (((long long)rand()) << 32);
		x = x | (int) curTimeMillis() | 0x80000000; // OR to make sure not zero
		if( clientCursors.count(x) == 0 )
			break;
	}
#if defined(_WIN32)
	cout << "alloccursorid " << x << endl;
#endif
	return x;
}

class CursInspector : public SingleResultObjCursor { 
	Cursor* clone() { 
		return new CursInspector(); 
	}
	void fill() { 
		b.append("byLocation_size", clientCursorsByLocation.size());
		b.append("clientCursors_size", clientCursorsById.size());

		stringstream ss;
		ss << '\n';
		int x = 40;
		DiskToCC::iterator it = clientCursorsByLocation.begin();
		while( it != clientCursorsByLocation.end() ) {
			DiskLoc dl = it->first;
			ss << dl.toString() << " -> \n";
			set<ClientCursor*>::iterator j = it->second.begin();
			while( j != it->second.end() ) {
				ss << "    cid:" << j->second->cursorid << ' ' << j->second->ns << " pos:" << j->second->pos << " LL:" << j->second->lastLoc.toString();
				try { 
					setClient(j->second->ns.c_str());
					Record *r = dl.rec();
					ss << " lwh:" << hex << r->lengthWithHeaders << " nxt:" << r->nextOfs << " prv:" << r->prevOfs << dec << ' ' << j->second->c->toString();
					if( r->nextOfs >= 0 && r->nextOfs < 16 ) 
						ss << " DELETED??? (!)";
				}
				catch(...) { 
					ss << " EXCEPTION";
				}
				ss << "\n";
				j++;
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

