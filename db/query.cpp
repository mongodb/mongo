// query.cpp

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"

//ns->query->DiskLoc
LRUishMap<JSObj,DiskLoc,5> lrutest(123);

int nextCursorId = 1;

JSObj emptyObj;

int getGtLtOp(Element& e);
void appendElementHandlingGtLt(JSObjBuilder& b, Element& e);

/* todo: _ cache query plans 
         _ use index on partial match with the query

   parameters
     query - the query, e.g., { name: 'joe' }
     order - order by spec, e.g., { name: 1 } 1=ASC, -1=DESC

*/
auto_ptr<Cursor> getIndexCursor(const char *ns, JSObj& query, JSObj& order) { 
	NamespaceDetails *d = nsdetails(ns);
	if( d == 0 ) return auto_ptr<Cursor>();

	// queryFields, e.g. { 'name' }
	set<string> queryFields;
	query.getFieldNames(queryFields);

	if( !order.isEmpty() ) {
		set<string> orderFields;
		order.getFieldNames(orderFields);
		// order by
		for(int i = 0; i < d->nIndexes; i++ ) { 
			JSObj idxInfo = d->indexes[i].info.obj(); // { name:, ns:, key: }
			assert( strcmp(ns, idxInfo.getStringField("ns")) == 0 );
			JSObj idxKey = idxInfo.getObjectField("key");
			set<string> keyFields;
			idxKey.getFieldNames(keyFields);
			if( keyFields == orderFields ) {
				bool reverse = 
					order.firstElement().type() == Number && 
					order.firstElement().number() < 0;
				JSObjBuilder b;
				/* todo: start with the right key, not just beginning of index, when query is also
				         specified!
				*/
				return auto_ptr<Cursor>(new BtreeCursor(d->indexes[i].head, reverse ? maxKey : emptyObj, reverse ? -1 : 1, false));
			}
		}
	}

	// regular query without order by
	for(int i = 0; i < d->nIndexes; i++ ) { 
		JSObj idxInfo = d->indexes[i].info.obj(); // { name:, ns:, key: }
		JSObj idxKey = idxInfo.getObjectField("key");
		set<string> keyFields;
		idxKey.getFieldNames(keyFields);
		if( keyFields == queryFields ) {
			JSObjBuilder b;
			JSObj q = query.extractFields(idxKey, b);
			/* regexp: only supported if form is /^text/ */
			JSObjBuilder b2;
			JSElemIter it(q);
			bool first = true;
			while( it.more() ) {
				Element e = it.next();
				if( e.eoo() )
					break;

				// GT/LT
				if( e.type() == Object ) { 
					int op = getGtLtOp(e);
					if( op ) { 
						if( !first || !it.next().eoo() ) {
							// compound keys with GT/LT not supported yet via index.
							goto fail;
						}
						int direction = - JSMatcher::opDirection(op);
						return auto_ptr<Cursor>( new BtreeCursor(
							d->indexes[i].head, 
							direction == 1 ? emptyObj : maxKey, 
							direction, 
							true) );
					}
				}

				first = false;
				if( e.type() == RegEx ) { 
					if( *e.regexFlags() )
						goto fail;
					const char *re = e.regex();
					const char *p = re;
					if( *p++ != '^' ) goto fail;
					while( *p ) {
						if( *p == ' ' || (*p>='0'&&*p<='9') || (*p>='@'&&*p<='Z') || (*p>='a'&&*p<='z') )
							;
						else
							goto fail;
						p++;
					}
					if( it.more() && !it.next().eoo() ) // we must be the last part of the key (for now until we are smarter)
						goto fail;
					// ok!
                    b2.append(e.fieldName(), re+1);
					break;
				}
				else {
					b2.append(e);
					//appendElementHandlingGtLt(b2, e);
				}
			}
			JSObj q2 = b2.done();
			return auto_ptr<Cursor>( 
				new BtreeCursor(d->indexes[i].head, q2, 1, true));
		}
	}

fail:
	return auto_ptr<Cursor>();
}

void deleteObjects(const char *ns, JSObj pattern, bool justOne) {
//	cout << "delete ns:" << ns << " queryobjsize:" << 
//		pattern.objsize() << endl;

	if( strstr(ns, ".system.") ) {
		cout << "ERROR: attempt to delete in system namespace " << ns << endl;
		return;
	}

	JSMatcher matcher(pattern);
	JSObj order;
	auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
	if( c.get() == 0 )
		c = theDataFileMgr.findAll(ns);
	while( c->ok() ) {
		Record *r = c->_current();
		DiskLoc rloc = c->currLoc();
		c->advance(); // must advance before deleting as the next ptr will die
		JSObj js(r);
		bool deep;
		if( !matcher.matches(js, &deep) ) {
			if( c->tempStopOnMiss() )
				break;
		}
		else { 
			assert( !deep || !c->dup(rloc) ); // can't be a dup, we deleted it!
//			cout << "  found match to delete" << endl;
			if( !justOne )
				c->noteLocation();
			theDataFileMgr.deleteRecord(ns, r, rloc);
			if( justOne )
				return;
			c->checkLocation();
		}
	}
}

struct Mod { 
	const char *fieldName;
	double n;
};

void applyMods(vector<Mod>& mods, JSObj obj) { 
	for( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ ) { 
		Mod& m = *i;
		Element e = obj.findElement(m.fieldName);
		if( e.type() == Number ) {
			e.number() += m.n;
		}
	}
}

/* get special operations like $inc 
   { $inc: { a:1, b:1 } }
*/
void getMods(vector<Mod>& mods, JSObj from) { 
	JSElemIter it(from);
	while( it.more() ) {
		Element e = it.next();
		if( strcmp(e.fieldName(), "$inc") == 0 && e.type() == Object ) {
			JSObj j = e.embeddedObject();
			JSElemIter jt(j);
			while( jt.more() ) { 
				Element f = jt.next();
				if( f.eoo() )
					break;
				Mod m;
				m.fieldName = f.fieldName();
				if( f.type() == Number ) {
					m.n = f.number();
					mods.push_back(m);
				} 
			}
		}
	}
}

/*
todo:
 smart requery find record immediately
*/
void updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert) {
//cout << "TEMP BAD";
//lrutest.find(updateobj);


	//	cout << "update ns:" << ns << " objsize:" << updateobj.objsize() << " queryobjsize:" << 
	//		pattern.objsize();

	if( strstr(ns, ".system.") ) { 
		cout << "\nERROR: attempt to update in system namespace " << ns << endl;
		return;
	}

	{
		JSMatcher matcher(pattern);
		JSObj order;
		auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
		if( c.get() == 0 )
			c = theDataFileMgr.findAll(ns);
		while( c->ok() ) {
			Record *r = c->_current();
			JSObj js(r);
			if( !matcher.matches(js) ) {
				if( c->tempStopOnMiss() )
					break;
			}
			else {
				/* note: we only update one row and quit.  if you do multiple later, 
				   be careful or multikeys in arrays could break things badly.  best 
				   to only allow updating a single row with a multikey lookup.
				   */

				/* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
				   regular ones at the moment. */
				if( updateobj.firstElement().fieldName()[0] == '$' ) {
					vector<Mod> mods;
					getMods(mods, updateobj);
					applyMods(mods, c->currLoc().obj());
					return;
				}

				theDataFileMgr.update(ns, r, c->currLoc(), updateobj.objdata(), updateobj.objsize());
				return;
			}
			c->advance();
		}
	}

	if( upsert ) {
		if( updateobj.firstElement().fieldName()[0] == '$' ) {
			/* upsert of an $inc. build a default */
			vector<Mod> mods;
			getMods(mods, updateobj);
			JSObjBuilder b;
			b.appendElements(pattern);
			for( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ )
				b.append(i->fieldName, i->n);
			JSObj obj = b.done();
			theDataFileMgr.insert(ns, (void*) obj.objdata(), obj.objsize());
			return;
		}
		theDataFileMgr.insert(ns, (void*) updateobj.objdata(), updateobj.objsize());
	}
}

int queryTraceLevel = 0;
int otherTraceLevel = 0;

// e.g.
//   system.cmd$.find( { queryTraceLevel: 2 } );
// 
// returns true if ran a cmd
//
inline bool runCommands(const char *ns, JSObj& jsobj, stringstream& ss, BufBuilder &b, JSObjBuilder& anObjBuilderForYa) { 
    if( strcmp(ns, "system.$cmd") != 0 ) 
		return false;

	ss << "\n  $cmd: " << jsobj.toString();

	bool ok = false;
	bool valid = false;

	Element e = jsobj.firstElement();
	if( e.eoo() ) goto done;
	if( e.type() == Number ) { 
		if( strcmp(e.fieldName(),"queryTraceLevel") == 0 ) {
			valid = ok = true;
			queryTraceLevel = (int) e.number();
		} else if( strcmp(e.fieldName(),"traceAll") == 0 ) { 
			valid = ok = true;
			queryTraceLevel = (int) e.number();
			otherTraceLevel = (int) e.number();
		}
	}
	else if( e.type() == String ) { 
		if( strcmp(e.fieldName(),"deleteIndexes") == 0 ) { 
			valid = true;
			/* note: temp implementation.  space not reclaimed! */
			NamespaceDetails *d = nsdetails(e.valuestr());
			cout << "CMD: deleteIndexes " << e.valuestr() << endl;
			if( d ) {
				ok = true;
				cout << "  d->nIndexes was " << d->nIndexes << endl;
				anObjBuilderForYa.append("nIndexesWas", (double)d->nIndexes);
				cout << "  temp implementation, space not reclaimed" << endl;
				d->nIndexes = 0;
			}
			else {
				anObjBuilderForYa.append("errmsg", "ns not found");
			}
		}
	}

done:
	if( !valid )
		anObjBuilderForYa.append("errmsg", "no such cmd");
	anObjBuilderForYa.append("ok", ok?1.0:0.0);
	JSObj x = anObjBuilderForYa.done();
	b.append((void*) x.objdata(), x.objsize());
	return true;
}

QueryResult* runQuery(const char *ns, int ntoreturn, JSObj jsobj, 
					  auto_ptr< set<string> > filter, stringstream& ss) {
	ss << "query:" << ns << " ntoreturn:" << ntoreturn;
	if( jsobj.objsize() > 100 ) 
		ss << " querysz:" << jsobj.objsize();
	if( queryTraceLevel >= 1 )
		cout << "query: " << jsobj.toString() << endl;

	int n = 0;
	BufBuilder b(32768);
	JSObjBuilder cmdResBuf;
	long long cursorid = 0;

	b.skip(sizeof(QueryResult));

	if( runCommands(ns, jsobj, ss, b, cmdResBuf) ) { 
		n = 1;
	}
	else {

		JSObj query = jsobj.getObjectField("query");
		JSObj order = jsobj.getObjectField("orderby");
		if( query.isEmpty() && order.isEmpty() )
			query = jsobj;

		auto_ptr<JSMatcher> matcher(new JSMatcher(query));

		auto_ptr<Cursor> c = getSpecialCursor(ns);
		if( c.get() == 0 ) {
			c = getIndexCursor(ns, query, order);
		}
		if( c.get() == 0 ) {
			c = theDataFileMgr.findAll(ns);
			if( queryTraceLevel >= 1 )
				cout << "  basiccursor" << endl;
		}

		int nscanned = 0;
		while( c->ok() ) {
			JSObj js = c->current();
			if( queryTraceLevel >= 50 )
				cout << " checking against:\n " << js.toString() << endl;
			nscanned++;
			bool deep;
			if( !matcher->matches(js, &deep) ) {
				if( c->tempStopOnMiss() )
					break;
			}
			else if( !deep || !c->dup(c->currLoc()) ) {
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
							ClientCursor::add(cc);
							cc->updateLocation();
							cc->filter = filter;
							break;
					}
				}
			}
			c->advance();
		}

		if( queryTraceLevel >=2 )
			cout << "  nscanned:" << nscanned << "\n  ";
	}

	QueryResult *qr = (QueryResult *) b.buf();
	qr->_data[0] = 0;
	qr->_data[1] = 0;
	qr->_data[2] = 0;
	qr->_data[3] = 0;
	qr->len = b.len();
	ss << " resLen:" << b.len();
	//	qr->channel = 0;
	qr->operation = opReply;
	qr->cursorId = cursorid;
	qr->startingFrom = 0;
	qr->nReturned = n;
	b.decouple();

	ss << " nReturned:" << n;
	return qr;
}

QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid) {

//	cout << "getMore ns:" << ns << " ntoreturn:" << ntoreturn << " cursorid:" << 
//		cursorid << endl;

	BufBuilder b(32768);

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
done:
				// done!  kill cursor.
				cursorid = 0;
				clientCursors.erase(it);
				delete cc;
				cc = 0;
				break;
			}
			JSObj js = c->current();
			bool deep;
			if( !cc->matcher->matches(js, &deep) ) {
				if( c->tempStopOnMiss() )
					goto done;
			} 
			else if( !deep || !c->dup(c->currLoc()) ) {
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
			c->advance();
		}
	}

	QueryResult *qr = (QueryResult *) b.buf();
	qr->cursorId = cursorid;
	qr->startingFrom = start;
	qr->len = b.len();
	//	qr->reserved = 0;
	qr->operation = opReply;
	qr->nReturned = n;
	b.decouple();

	return qr;
}
