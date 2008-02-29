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

typedef map<DiskLoc, ClientCursor*> DiskToCC;
map<DiskLoc, ClientCursor*> byLocation;
//HashTable<DiskLoc,ClientCursor*> byLocation(malloc(10000000), 10000000, "bylocation");

CCMap clientCursors;

class CursInspector : public SingleResultObjCursor { 
	Cursor* clone() { 
		return new CursInspector(); 
	}
//	Cursor* clone() { return new CursInspector(*this); }
	void fill() { 
		b.append("byLocation_size", byLocation.size());
		b.append("clientCursors_size", clientCursors.size());

cout << byLocation.size() << endl;

		stringstream ss;
		ss << '\n';
		int x = 40;
		DiskToCC::iterator it = byLocation.begin();
		while( it != byLocation.end() ) {
			DiskLoc dl = it->first;
			ClientCursor *cc = it->second;
			ss << dl.toString() << " -> \n";

			while( cc ) {
				ss << "    cid:" << cc->cursorid << ' ' << cc->ns << " pos:" << cc->pos << " LL:" << cc->lastLoc.toString();
				try { 
					setClient(cc->ns.c_str());
					Record *r = dl.rec();
					ss << " lwh:" << hex << r->lengthWithHeaders << " nxt:" << r->nextOfs << " prv:" << r->prevOfs << dec << ' ' << cc->c->toString();
					if( r->nextOfs >= 0 && r->nextOfs < 16 ) 
						ss << " DELETED??? (!)";
				}
				catch(...) { 
					ss << " EXCEPTION";
				}
				ss << "\n";
				cc = cc->nextAtThisLocation;
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
	DiskToCC::iterator it = byLocation.find(dl);
//	cout << "atd:" << dl.toString() << endl;
	if( it != byLocation.end() ) {
		ClientCursor *cc = it->second;
		byLocation.erase(it);

		assert( cc != 0 );
		int z = 0;
		while( cc ) { 
			z++;
//			cout << "cc: " << cc->ns << endl;
			ClientCursor *nxt = cc->nextAtThisLocation;
			cc->nextAtThisLocation = 0; // updateLocation will manipulate linked list ptrs, so clean that up first.
			cc->c->checkLocation();
			cc->c->advance();
			cc->lastLoc.Null(); // so updateLocation doesn't try to remove, just to be faster -- we handled that.
			cc->updateLocation();
			cc = nxt;
		}
//		cout << "z:" << z << endl;
	}
}

void ClientCursor::cleanupByLocation(DiskLoc loc, long long cursorid) { 
	if( loc.isNull() )
		return;

	DiskToCC::iterator it = byLocation.find(loc);
	if( it != byLocation.end() ) {
		ClientCursor *first = it->second;
		ClientCursor *cc = first;
		ClientCursor *prev = 0;

		while( 1 ) {
			if( cc == 0 ) 
				break;
			if( cc->cursorid == cursorid ) { 
				// found one to remove.
				if( prev == 0 ) { 
					if( cc->nextAtThisLocation )
						byLocation[loc] = cc->nextAtThisLocation;
					else
						byLocation.erase(it);
				}
				else { 
					prev->nextAtThisLocation = cc->nextAtThisLocation;
				}
				break;
			}
			cc = cc->nextAtThisLocation;
		}
	}
}

ClientCursor::~ClientCursor() {
	assert( pos != -2 );

	cleanupByLocation(lastLoc, cursorid);

	assert( pos != -2 );

	// defensive
	lastLoc.Null();
	cursorid = -1; 
	pos = -2;
	nextAtThisLocation = 0;
}

// note this doesn't set lastLoc -- caller should.
void ClientCursor::addToByLocation(DiskLoc cl) { 
	if( nextAtThisLocation ) { 
		wassert( nextAtThisLocation == 0 );
		return;
	}

	DiskToCC::iterator j = byLocation.find(cl);
	nextAtThisLocation = j == byLocation.end() ? 0 : j->second;
	byLocation[cl] = this;
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

/* report to us that a new clientcursor exists so we can track it. 
   note you still must call updateLocation (which likely should be changed)
*/
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
		x = x | (int) curTimeMillis() | 0x80000000; // last or to w make sure not zero
		if( clientCursors.count(x) == 0 )
			break;
	}
	return x;
}
