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
//	cout << "TEMP delete ns:" << ns << " queryobjsize:" << 
//		pattern.objsize() << endl;

	if( strstr(ns, ".system.") ) {
		if( strstr(ns, ".system.indexes") ) {
			cout << "WARNING: delete on system namespace " << ns << endl;
		}
		else { 
			cout << "ERROR: attempt to delete in system namespace " << ns << endl;
			return;
		}
	}

	JSMatcher matcher(pattern);
	JSObj order;
	auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
	if( c.get() == 0 )
		c = theDataFileMgr.findAll(ns);

	Cursor &tempDebug = *c;
	int temp = 0;
	int tempd = 0;

DiskLoc _tempDelLoc;

	while( c->ok() ) {
		temp++;

		Record *r = c->_current();
		DiskLoc rloc = c->currLoc();
		c->advance(); // must advance before deleting as the next ptr will die
		JSObj js(r);
		//cout << "TEMP: " << js.toString() << endl;
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
_tempDelLoc = rloc;
			theDataFileMgr.deleteRecord(ns, r, rloc);
			tempd = temp;
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

int initialExtentSize(int len);

void clean(const char *ns, NamespaceDetails *d) {
//	d->firstExtent.Null();
//	d->lastExtent.Null();
	for( int i = 0; i < Buckets; i++ ) 
		d->deletedList[i].Null();
//	d->datasize = 0; d->nrecords = 0; d->lastExtentSize = 0; d->nIndexes = 0;
//	client->newestFile()->newExtent(ns, initialExtentSize(500));
}

string validateNS(const char *ns, NamespaceDetails *d) {
	stringstream ss;
	ss << "validate...\n";

	try { 

		auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
		int n = 0;
		long long len = 0;
		long long nlen = 0;
		while( c->ok() ) { 
			n++;
			Record *r = c->_current();
			len += r->lengthWithHeaders;
			nlen += r->netLength();
			c->advance();
		}
		ss << "  " << n << " objects found\n";
		ss << "  " << len << " bytes record data w/headers\n";
		ss << "  " << nlen << " bytes record data wout/headers\n";

		ss << "  deletedList: ";
		for( int i = 0; i < Buckets; i++ ) { 
			ss << (d->deletedList[i].isNull() ? '0' : '1');
		}
		ss << endl;
		int ndel = 0;
		long long delSize = 0;
		for( int i = 0; i < Buckets; i++ ) { 
			DiskLoc loc = d->deletedList[i];
			while( !loc.isNull() ) { 
				ndel++;
				DeletedRecord *d = loc.drec();
				delSize += d->lengthWithHeaders;
				loc = d->nextDeleted;
			}
		}
		ss << "  deleted: n: " << ndel << " size: " << delSize << endl;

	}
	catch(AssertionException) {
		ss << "\n  exception during validate\n" << endl; 
	}

	return ss.str();
}

// e.g.
//   system.cmd$.find( { queryTraceLevel: 2 } );
// 
// returns true if ran a cmd
//
inline bool runCommands(const char *ns, JSObj& jsobj, stringstream& ss, BufBuilder &b, JSObjBuilder& anObjBuilderForYa) { 

	const char *p = strchr(ns, '.');
	if( !p ) return false;
	if( strcmp(p, ".$cmd") != 0 ) return false;

	ss << "\n  $cmd: " << jsobj.toString();

	bool ok = false;
	bool valid = false;

	Element e = jsobj.firstElement();
	if( e.eoo() ) goto done;
	if( e.type() == Number ) { 
		if( strncmp(ns, "admin", p-ns) != 0 ) // admin only
			return false;
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
		string us(ns, p-ns);

		if( strcmp( e.fieldName(), "clean") == 0 ) { 
			valid = true;
			string dropNs = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(dropNs.c_str());
			cout << "CMD: clean " << dropNs << endl;
			if( d ) { 
				ok = true;
				anObjBuilderForYa.append("ns", dropNs.c_str());
				clean(dropNs.c_str(), d);
			}
			else {
				anObjBuilderForYa.append("errmsg", "ns not found");
			}
		}
		else if( strcmp( e.fieldName(), "drop") == 0 ) { 
			valid = true;
			string dropNs = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(dropNs.c_str());
			cout << "CMD: clean " << dropNs << endl;
			if( d ) { 
				ok = true;
				anObjBuilderForYa.append("ns", dropNs.c_str());
				client->namespaceIndex->kill(dropNs.c_str());
			}
			else {
				anObjBuilderForYa.append("errmsg", "ns not found");
			}
		}
		else if( strcmp( e.fieldName(), "validate") == 0 ) { 
			valid = true;
			string toValidateNs = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(toValidateNs.c_str());
			cout << "CMD: validate " << toValidateNs << endl;
			if( d ) { 
				ok = true;
				anObjBuilderForYa.append("ns", toValidateNs.c_str());
				string s = validateNS(toValidateNs.c_str(), d);
				anObjBuilderForYa.append("result", s.c_str());
			}
			else {
				anObjBuilderForYa.append("errmsg", "ns not found");
			}
		}
		else if( strcmp(e.fieldName(),"deleteIndexes") == 0 ) { 
			valid = true;
			/* note: temp implementation.  space not reclaimed! */
			string toDeleteNs = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
			cout << "CMD: deleteIndexes " << toDeleteNs << endl;
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

int nCaught = 0;

QueryResult* runQuery(const char *ns, int ntoskip, int _ntoreturn, JSObj jsobj, 
					  auto_ptr< set<string> > filter, stringstream& ss) 
{
	bool wantMore = true;
	int ntoreturn = _ntoreturn;
	if( _ntoreturn < 0 ) { 
		ntoreturn = -_ntoreturn;
		wantMore = false;
	}

	ss << "query:" << ns << " _ntoreturn:" << _ntoreturn;
	if( ntoskip ) ss << " ntoskip:" << ntoskip;
	if( jsobj.objsize() > 100 ) 
		ss << " querysz:" << jsobj.objsize();
	if( queryTraceLevel >= 1 )
		cout << "query: " << jsobj.toString() << endl;

	int n = 0;
	BufBuilder b(32768);
	JSObjBuilder cmdResBuf;
	long long cursorid = 0;

	b.skip(sizeof(QueryResult));

	/* we assume you are using findOne() for running a cmd... */
	if( ntoreturn == 1 && runCommands(ns, jsobj, ss, b, cmdResBuf) ) { 
		n = 1;
	}
	else {

		JSObj query = jsobj.getObjectField("query");
		JSObj order = jsobj.getObjectField("orderby");
		if( query.isEmpty() && order.isEmpty() )
			query = jsobj;

		auto_ptr<JSMatcher> matcher(new JSMatcher(query));
JSMatcher &debug1 = *matcher;
assert( debug1.getN() < 5000 );

		int nscanned = 0;
		auto_ptr<Cursor> c = getSpecialCursor(ns);

		/*try*/{

			if( c.get() == 0 ) {
				c = getIndexCursor(ns, query, order);
			}
			if( c.get() == 0 ) {
				c = theDataFileMgr.findAll(ns);
				if( queryTraceLevel >= 1 )
					cout << "  basiccursor" << endl;
			}

			while( c->ok() ) {
				JSObj js = c->current();
				if( queryTraceLevel >= 50 )
					cout << " checking against:\n " << js.toString() << endl;
				nscanned++;
				bool deep;

JSMatcher &debug = *matcher;
assert( debug.getN() < 5000 );

				if( !matcher->matches(js, &deep) ) {
					if( c->tempStopOnMiss() )
						break;
				}
				else if( !deep || !c->dup(c->currLoc()) ) { // i.e., check for dups on deep items only
					// got a match.
					if( ntoskip > 0 ) {
						ntoskip--;
					}
					else {
						bool ok = true;
						if( filter.get() ) {
							// we just want certain fields from the object.
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
									/* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
									if( wantMore && ntoreturn != 1 ) {
										c->advance();
										if( c->ok() ) {
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
										}
									}
									break;
							}
						}
					}
				}
				c->advance();
			}

			if( queryTraceLevel >=2 )
				cout << "  nscanned:" << nscanned << "\n  ";
		}
		/*catch( AssertionException e ) { 
			if( n )
				throw e;
			if( nCaught++ >= 1000 ) { 
				cout << "Too many query exceptions, terminating" << endl;
				exit(-8);
			}
			cout << " Assertion running query, returning an empty result" << endl;
		}*/
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

void killCursors(int n, long long *ids) {
	int k = 0;
	for( int i = 0; i < n; i++ ) {
		if( ClientCursor::erase(ids[i]) )
			k++;
	}
	cout << "killCursors: found " << k << " of " << n << endl;
}

QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid) {

//	cout << "getMore ns:" << ns << " ntoreturn:" << ntoreturn << " cursorid:" << 
//		cursorid << endl;

	BufBuilder b(32768);

	ClientCursor *cc = ClientCursor::find(cursorid);

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
				bool ok = ClientCursor::erase(cursorid);
				assert(ok);
				cursorid = 0;
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
						c->advance();
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
