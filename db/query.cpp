// query.cpp

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"

int nextCursorId = 1;

void deleteObjects(const char *ns, JSObj pattern, bool justOne) {
	cout << "delete ns:" << ns << " queryobjsize:" << 
		pattern.objsize() << endl;

	JSMatcher matcher(pattern);

	Cursor c = theDataFileMgr.findAll(ns);
	while( c.ok() ) {
		Record *r = c.current();
		DiskLoc rloc = c.curr;
		c.advance(); // must advance before deleting as the next ptr will die
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

	JSMatcher matcher(pattern);

	Cursor c = theDataFileMgr.findAll(ns);
	while( c.ok() ) {
		Record *r = c.current();
		JSObj js(r);
		if( matcher.matches(js) ) {
			cout << "  found match to update" << endl;
			theDataFileMgr.update(ns, r, c.curr, updateobj.objdata(), updateobj.objsize());
			return;
		}
		c.advance();
	}

	cout << "  no match found. ";
	if( upsert )
		cout << "doing upsert.";
	cout << endl;
	if( upsert )
		theDataFileMgr.insert(ns, (void*) updateobj.objdata(), updateobj.objsize());
}

QueryResult* runQuery(const char *ns, int ntoreturn, JSObj jsobj) {

	cout << "runQuery ns:" << ns << " ntoreturn:" << ntoreturn << " queryobjsize:" << 
		jsobj.objsize() << endl;

	BufBuilder b;
	JSMatcher matcher(jsobj);

	QueryResult *qr = 0;
	b.skip(sizeof(QueryResult));

	int n = 0;
	Cursor c = theDataFileMgr.findAll(ns);
	while( c.ok() ) {
		Record *r = c.current();

		JSObj js(r);
		if( matcher.matches(js) ) {
			assert( js.objsize() <= r->netLength() );
			b.append(r->data, js.objsize());
			n++;
			if( n >= ntoreturn && ntoreturn != 0 )
				break;
		}

		c.advance();
	}

	qr = (QueryResult *) b.buf();
	qr->len = b.len();
	qr->reserved = 0;
	qr->operation = opReply;
	qr->cursorId = 0; //nextCursorId++;
	qr->startingFrom = 0;
	qr->nReturned = n;
	b.decouple();

	return qr;
}
