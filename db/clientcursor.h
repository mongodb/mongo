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
	DiskLoc _lastLoc; // use getter and setter not this.
	static CursorId allocCursorId();
public:
	ClientCursor() : cursorid( allocCursorId() ), pos(0) { 
		clientCursorsById.insert( make_pair(cursorid, this) );
	}
	~ClientCursor();
	const CursorId cursorid;
	string ns;
	auto_ptr<JSMatcher> matcher;
	auto_ptr<Cursor> c;
	int pos;
	DiskLoc lastLoc() const { return _lastLoc; }
	void setLastLoc(DiskLoc);
	auto_ptr< set<string> > filter; // which fields query wants returned
	Message originalMessage; // this is effectively an auto ptr for data the matcher points to.

	/* Get rid of cursors for namespaces that begin with nsprefix. 
	   Used by drop, deleteIndexes, dropDatabase.
	*/
	static void invalidate(const char *nsPrefix);

	static bool erase(CursorId id) { 
		ClientCursor *cc = find(id);
		if( cc ) {
			delete cc; 
			return true;
		}
		return false;
	}

	static ClientCursor* find(CursorId id, bool warn = true) {
		CCById::iterator it = clientCursorsById.find(id);
		if( it == clientCursorsById.end() ) { 
			if( warn ) 
				OCCASIONALLY cout << "ClientCursor::find(): cursor not found in map " << id << " (ok after a drop)\n";
			return 0;
		}
		return it->second;
	}

	/* call when cursor's location changes so that we can update the 
	   cursorsbylocation map.  if you are locked and internally iterating, only 
	   need to call when you are ready to "unlock".
	   */
	void updateLocation();

	void cleanupByLocation(DiskLoc loc);
};
