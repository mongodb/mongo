// query.cpp

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"

int nextCursorId = 1;

QueryResult* runQuery(const char *ns, int ntoreturn, JSObj jsobj) {

	cout << "runQuery ns:" << ns << " ntoreturn:" << ntoreturn << " queryobjsize:" << 
		jsobj.objsize() << endl;

	/* temp implementation -- just returns everything! */

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
			b.append(r->netLength()+4);
			b.append(r->data, r->netLength());
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
