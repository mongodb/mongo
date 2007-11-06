// query.cpp

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"

int nextCursorId = 1;

void deleteObjects(const char *ns, JSObj pattern, bool justOne) {
	cout << "delete ns:" << ns << " queryobjsize:" << 
		pattern.objsize() << endl;

	if( strncmp(ns, "system.", 7) == 0 ) { 
		cout << "ERROR: attempt to delete in system namespace " << ns << endl;
		return;
	}

	JSMatcher matcher(pattern);

	auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
	while( c->ok() ) {
		Record *r = c->_current();
		DiskLoc rloc = c->currLoc();
		c->advance(); // must advance before deleting as the next ptr will die
		JSObj js(r);
		if( matcher.matches(js) ) {
			cout << "  found match to delete" << endl;
			theDataFileMgr.deleteRecord(ns, r, rloc);
			if( justOne )
				return;
		}
	}
}

void updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert) {
	cout << "update ns:" << ns << " objsize:" << updateobj.objsize() << " queryobjsize:" << 
		pattern.objsize() << endl;

	if( strncmp(ns, "system.", 7) == 0 ) { 
		cout << "ERROR: attempt to update in system namespace " << ns << endl;
		return;
	}

	JSMatcher matcher(pattern);

	auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
	while( c->ok() ) {
		Record *r = c->_current();
		JSObj js(r);
		if( matcher.matches(js) ) {
			cout << "  found match to update" << endl;
			theDataFileMgr.update(ns, r, c->currLoc(), updateobj.objdata(), updateobj.objsize());
			return;
		}
		c->advance();
	}

	cout << "  no match found. ";
	if( upsert )
		cout << "doing upsert.";
	cout << endl;
	if( upsert )
		theDataFileMgr.insert(ns, (void*) updateobj.objdata(), updateobj.objsize());
}

map<DiskLoc, ClientCursor*> cursorsByLocation;
typedef map<long long, ClientCursor*> CCMap;
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

/* must call this on a delete so we clean up the cursors. */
void aboutToDelete(const DiskLoc& dl) { 
	map<DiskLoc,ClientCursor*>::iterator it = cursorsByLocation.find(dl);
	if( it != cursorsByLocation.end() ) {
		ClientCursor *cc = it->second;
		assert( !cc->c->eof() );
		cc->c->advance();
		cc->updateLocation();
	}
}

ClientCursor::~ClientCursor() {
	if( !lastLoc.isNull() ) { 
		int n = cursorsByLocation.erase(lastLoc);
		assert( n == 1 );
	}
	lastLoc.Null();
}

void ClientCursor::updateLocation() {
	if( !lastLoc.isNull() ) { 
		int n = cursorsByLocation.erase(lastLoc);
		assert( n == 1 );
	}
	if( !c->currLoc().isNull() )
		cursorsByLocation[c->currLoc()] = this;
	lastLoc = c->currLoc();
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

QueryResult* runQuery(const char *ns, int ntoreturn, JSObj jsobj, auto_ptr< set<string> > filter) {

	cout << "runQuery ns:" << ns << " ntoreturn:" << ntoreturn << " queryobjsize:" << 
		jsobj.objsize() << endl;

	BufBuilder b;

	auto_ptr<JSMatcher> matcher(new JSMatcher(jsobj));

	QueryResult *qr = 0;
	b.skip(sizeof(QueryResult));

	int n = 0;

	auto_ptr<Cursor> c = getSpecialCursor(ns);
	if( c.get() == 0 )
		c = theDataFileMgr.findAll(ns);

	long long cursorid = 0;
	while( c->ok() ) {
		JSObj js = c->current();
		if( matcher->matches(js) ) {
			bool ok = true;
			if( filter.get() ) {
				JSObj x;
				ok = x.addFields(js, *filter) > 0;
				if( ok ) 
					b.append((void*) x.objdata(), x.objsize());
			}
			else {
				b.append((void*) js.objdata(), js.objsize());
			}
			if( ok ) {
				n++;
				if( (ntoreturn>0 && (n >= ntoreturn || b.len() > 16*1024*1024)) ||
					(ntoreturn==0 && b.len()>1*1024*1024) ) {
					// more...so save a cursor
					ClientCursor *cc = new ClientCursor();
					cc->c = c;
					cursorid = allocCursorId();
					cc->cursorid = cursorid;
					cc->matcher = matcher;
					cc->ns = ns;
					cc->pos = n;
					clientCursors[cursorid] = cc;
					cc->updateLocation();
					cc->filter = filter;
					break;
				}
			}
		}
		c->advance();
	}

	qr = (QueryResult *) b.buf();
	qr->len = b.len();
	qr->reserved = 0;
	qr->operation = opReply;
	qr->cursorId = cursorid;
	qr->startingFrom = 0;
	qr->nReturned = n;
	b.decouple();

	return qr;
}

QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid) {

	cout << "getMore ns:" << ns << " ntoreturn:" << ntoreturn << " cursorid:" << 
		cursorid << endl;

	BufBuilder b;

	ClientCursor *cc = 0;
	CCMap::iterator it = clientCursors.find(cursorid);
	if( it == clientCursors.end() ) { 
		cout << "Cursor not found in map.  cursorid: " << cursorid << endl;
	}
	else {
		cc = it->second;
	}

	b.skip(sizeof(QueryResult));

	int start = 0;
	int n = 0;

	if( cc ) {
		start = cc->pos;
		Cursor *c = cc->c.get();
		while( 1 ) {
			if( !c->ok() ) {
				// done!  kill cursor.
				cursorid = 0;
				clientCursors.erase(it);
				delete cc;
				cc = 0;
				break;
			}
			JSObj js = c->current();
			if( cc->matcher->matches(js) ) {
				bool ok = true;
				if( cc->filter.get() ) {
					JSObj x;
					ok = x.addFields(js, *cc->filter) > 0;
					if( ok ) 
						b.append((void*) x.objdata(), x.objsize());
				}
				else {
					b.append((void*) js.objdata(), js.objsize());
				}
				if( ok ) {
					n++;
					if( (ntoreturn>0 && (n >= ntoreturn || b.len() > 16*1024*1024)) ||
						(ntoreturn==0 && b.len()>1*1024*1024) ) {
						cc->pos += n;
						cc->updateLocation();
						break;
					}
				}
			}
		}
		c->advance();
	}

	QueryResult *qr = (QueryResult *) b.buf();
	qr->cursorId = cursorid;
	qr->startingFrom = start;
	qr->len = b.len();
	qr->reserved = 0;
	qr->operation = opReply;
	qr->nReturned = n;
	b.decouple();

	return qr;
}
