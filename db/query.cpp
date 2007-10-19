// query.cpp

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"

int nextCursorId = 1;

QueryResult* runQuery(const char *ns, const char *query, int ntoreturn) {

	/* temp implementation -- just returns everything! */

	BufBuilder b;

	QueryResult *qr = 0;
	b.skip(qr->data - ((char *)qr));

	int n = 0;
	Cursor c = theDataFileMgr.findAll(ns);
	while( c.ok() ) {
		Record *r = c.current();

		JSObj js(r);
		// check criteria here.

		b.append(r->netLength()+4);
		b.append(r->data, r->netLength());
		n++;

		if( n >= ntoreturn )
			break;

		c.advance();
	}

	qr = (QueryResult *) b.buf();
	qr->len = b.len();
	qr->reserved = 0;
	qr->operation = opReply;
	qr->cursorId = nextCursorId++;
	qr->startOfs = 0;
	qr->nReturned = n;
	b.decouple();

	return qr;
}
