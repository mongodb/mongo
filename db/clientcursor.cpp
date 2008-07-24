/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* clientcursor.cpp

   ClientCursor is a wrapper that represents a cursorid from our client 
   application's perspective.

   Cursor -- and its derived classes -- are our internal cursors.
*/

#include "stdafx.h"
#include "query.h"
#include "introspect.h"
#include <time.h>

/* TODO: FIX cleanup of clientCursors when hit the end. (ntoreturn insufficient) */

CCById clientCursorsById;

/* ------------------------------------------- */

typedef multimap<DiskLoc, ClientCursor*> ByLoc;
ByLoc byLoc;

void ClientCursor::setLastLoc(DiskLoc L) { 
	if( L == _lastLoc ) 
		return;

	if( !_lastLoc.isNull() ) {
		ByLoc::iterator i = kv_find(byLoc, _lastLoc, this);
		if( i != byLoc.end() )
			byLoc.erase(i);
	}

	if( !L.isNull() )
		byLoc.insert( make_pair(L, this) );
	_lastLoc = L;
}

/* ------------------------------------------- */

/* must call this when a btree node is updated */
void removedKey(const DiskLoc& btreeLoc, int keyPos) { 
// TODO!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
}

/* todo: this implementation is incomplete.  we use it as a prefix for dropDatabase, which 
         works fine as the prefix will end with '.'.  however, when used with drop and 
		 deleteIndexes, this could take out cursors that belong to something else -- if you 
		 drop "foo", currently, this will kill cursors for "foobar".
*/
void ClientCursor::invalidate(const char *nsPrefix) { 
	vector<ClientCursor*> toDelete;

	int len = strlen(nsPrefix);
	assert( len > 0 && strchr(nsPrefix, '.') );
	for( ByLoc::iterator i = byLoc.begin(); i != byLoc.end(); ++i ) {
		ClientCursor *cc = i->second;
		if( strncmp(nsPrefix, cc->ns.c_str(), len) == 0 )
			toDelete.push_back(i->second);
	}

	for( vector<ClientCursor*>::iterator i = toDelete.begin(); i != toDelete.end(); ++i ) 
		delete (*i);
}

/* must call this on a delete so we clean up the cursors. */
void aboutToDelete(const DiskLoc& dl) { 
	vector<ClientCursor*> toAdvance;

	for( ByLoc::iterator i = byLoc.lower_bound(dl); 
		i != byLoc.upper_bound(dl); ++i ) { 
			toAdvance.push_back(i->second);
	}

	for( vector<ClientCursor*>::iterator i = toAdvance.begin();
		i != toAdvance.end(); ++i ) 
	{ 
		(*i)->c->checkLocation();
		(*i)->c->advance();
		wassert( (*i)->c->currLoc() != dl );
		(*i)->updateLocation();
	}
}

ClientCursor::~ClientCursor() {
	assert( pos != -2 );
	setLastLoc( DiskLoc() ); // removes us from bylocation multimap
	clientCursorsById.erase(cursorid);
	// defensive:
	(CursorId&) cursorid = -1; 
	pos = -2;
}

/* call when cursor's location changes so that we can update the 
   cursorsbylocation map.  if you are locked and internally iterating, only 
   need to call when you are ready to "unlock".
*/
void ClientCursor::updateLocation() {
	assert( cursorid );
	DiskLoc cl = c->currLoc();
	if( lastLoc() == cl ) {
		cout << "info: lastloc==curloc " << ns << '\n';
		return;
	}
	setLastLoc(cl);
	c->noteLocation();
}

int ctmLast = 0; // so we don't have to do find() which is a little slow very often.
long long ClientCursor::allocCursorId() { 
	long long x;
	int ctm = (int) curTimeMillis();
	while( 1 ) {
		x = (((long long)rand()) << 32);
		x = x | ctm | 0x80000000; // OR to make sure not zero
		if( ctm != ctmLast || ClientCursor::find(x, false) == 0 )
			break;
	}
	ctmLast = ctm;
	DEV cout << "  alloccursorid " << x << endl;
	return x;
}

class CursInspector : public SingleResultObjCursor { 
	Cursor* clone() { 
		return new CursInspector(); 
	}
	void fill() { 
		b.append("byLocation_size", byLoc.size());
		b.append("clientCursors_size", clientCursorsById.size());
/* todo update for new impl:
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
*/
	}
public:
	CursInspector() { reg("intr.cursors"); }
} _ciproto;
