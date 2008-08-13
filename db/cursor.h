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

#pragma once

#include "../stdafx.h"

/* Query cursors, base class.  This is for our internal cursors.  "ClientCursor" is a separate 
   concept and is for the user's cursor.
*/
class Cursor {
public:
	virtual bool ok() = 0;
	bool eof() { return !ok(); }
	virtual Record* _current() = 0;
	virtual JSObj current() = 0;
	virtual DiskLoc currLoc() = 0;
	virtual bool advance() = 0; /*true=ok*/

	/* Implement these if you want the cursor to be "tailable" */
	/* tailable(): if true, cursor has tailable capability AND
	               the user requested use of those semantics. */
	virtual bool tailable() { return false; } 
	/* indicates we should mark where we are and go into tail mode. */
	virtual void setAtTail() { assert(false); }
	/* you must call tailResume before reusing the cursor */
	virtual void tailResume() { }
	/* indicates ifi we are actively tailing.  once it goes active,
	   this should return treu even after tailResume(). */
	virtual bool tailing() { return false; } 

	virtual void aboutToDeleteBucket(const DiskLoc& b) { }

	/* optional to implement.  if implemented, means 'this' is a prototype */
	virtual Cursor* clone() { return 0; }

	virtual bool tempStopOnMiss() { return false; }

	/* called after every query block is iterated -- i.e. between getMore() blocks
	   so you can note where we are, if necessary.
	   */
	virtual void noteLocation() { } 

	/* called before query getmore block is iterated */
	virtual void checkLocation() { } 

	virtual const char * toString() { return "abstract?"; }

	/* used for multikey index traversal to avoid sending back dups. see JSMatcher::matches() */
	set<DiskLoc> dups;
	bool getsetdup(DiskLoc loc) {
		/* to save mem only call this when there is risk of dups (e.g. when 'deep'/multikey) */
		if( dups.count(loc) > 0 )
			return true;
		dups.insert(loc);
		return false;
	}
};

/* table-scan style cursor */
class BasicCursor : public Cursor {
protected:
	DiskLoc curr, last;

private:
	// for tailing:
	enum State { Normal, TailPoint, TailResumed } state;
	void init() { state = Normal; }

public:
	bool ok() { return !curr.isNull(); }
	Record* _current() {
		assert( ok() );
		return curr.rec();
	}
	JSObj current() { 
		Record *r = _current();
		JSObj j(r);
		return j;
	}
	virtual DiskLoc currLoc() { return curr; }

	bool advance() { 
		if( eof() )
			return false;
		Record *r = _current();
		last = curr;
		curr = r->getNext(curr);
		return ok();
	}

	BasicCursor(DiskLoc dl) : curr(dl) { init(); }
	BasicCursor() { init(); }
	virtual const char * toString() { return "BasicCursor"; }

	virtual void tailResume() { 
		if( state == TailPoint ) { 
			state = TailResumed;
			advance();
		}
	}
	virtual void setAtTail() { 
		assert( state != TailPoint ); 
		assert( curr.isNull() );
		assert( !last.isNull() );
		curr = last; last.Null();
		state = TailPoint;
	}
	virtual bool tailable() { 
		// to go into tail mode we need a non-null point of reference for resumption
		return !last.isNull(); 
	}
	virtual bool tailing() { 
		return state != Normal; 
	}
};

/* used for order { $natural: -1 } */
class ReverseCursor : public BasicCursor {
public:
	bool advance() { 
		if( eof() )
			return false;
		Record *r = _current();
		last = curr;
		curr = r->getPrev(curr);
		return ok();
	}

	ReverseCursor(DiskLoc dl) : BasicCursor(dl) { }
	ReverseCursor() { }
	virtual const char * toString() { return "ReverseCursor"; }
};

