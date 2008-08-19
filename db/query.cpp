// query.cpp

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

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "javajs.h"
#include "json.h"
#include "repl.h"
#include "scanandorder.h"

/* We cut off further objects once we cross this threshold; thus, you might get 
   a little bit more than this, it is a threshold rather than a limit.
*/
const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

//ns->query->DiskLoc
LRUishMap<JSObj,DiskLoc,5> lrutest(123);

int nextCursorId = 1;
extern bool useCursors;

#pragma pack(push,1)
struct EmptyObject {
	EmptyObject() { len = 5; jstype = EOO; }
	int len;
	char jstype;
} emptyObject;
#pragma pack(pop)

JSObj emptyObj((char *) &emptyObject);

int getGtLtOp(Element& e);
void appendElementHandlingGtLt(JSObjBuilder& b, Element& e);
int runCount(const char *ns, JSObj& cmd, string& err);

/* todo: _ cache query plans 
         _ use index on partial match with the query

   parameters
     query - the query, e.g., { name: 'joe' }
     order - order by spec, e.g., { name: 1 } 1=ASC, -1=DESC
     simpleKeyMatch - set to true if the query is purely for a single key value
                      unchanged otherwise.
*/
auto_ptr<Cursor> getIndexCursor(const char *ns, JSObj& query, JSObj& order, bool *simpleKeyMatch = 0, bool *isSorted = 0) { 
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
				DEV cout << " using index " << d->indexes[i].indexNamespace() << '\n';
				if( isSorted )
					*isSorted = true;
				return auto_ptr<Cursor>(new BtreeCursor(d->indexes[i], reverse ? maxKey : emptyObj, reverse ? -1 : 1, false));
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
			bool simple = true;
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
						if( op >= JSMatcher::opIN ) {
							// $in does not use an index (at least yet, should when # of elems is tiny)
							goto fail;
						}

						{
							JSElemIter k(e.embeddedObject());
							k.next();
							if( !k.next().eoo() ) { 
								/* compound query like { $lt : 9, $gt : 2 } 
								   for those our method below won't work.
								   need more work on "stopOnMiss" in general -- may
								   be issues with it.  so fix this to use index after
								   that is fixed. 
								*/
								OCCASIONALLY cout << "finish query optimizer for lt gt compound\n";
								goto fail;
							}
						}

						int direction = - JSMatcher::opDirection(op);
						return auto_ptr<Cursor>( new BtreeCursor(
							d->indexes[i], 
							direction == 1 ? emptyObj : maxKey, 
							direction, 
							true) );
					}
				}

				first = false;
				if( e.type() == RegEx ) { 
					simple = false;
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
			DEV cout << "using index " << d->indexes[i].indexNamespace() << endl;
			if( simple && simpleKeyMatch ) *simpleKeyMatch = true;
			return auto_ptr<Cursor>( 
				new BtreeCursor(d->indexes[i], q2, 1, true));
		}
	}

fail:
	DEV cout << "getIndexCursor fail " << ns << '\n';
	return auto_ptr<Cursor>();
}

/* ns:      namespace, e.g. <client>.<collection>
   pattern: the "where" clause / criteria
   justOne: stop after 1 match
*/
int deleteObjects(const char *ns, JSObj pattern, bool justOne, bool god) {
	if( strstr(ns, ".system.") && !god ) {
		/*if( strstr(ns, ".system.namespaces") ){ 
			cout << "info: delete on system namespace " << ns << '\n';
		}
		else if( strstr(ns, ".system.indexes") ) {
			cout << "info: delete on system namespace " << ns << '\n';
		}
		else*/ { 
			cout << "ERROR: attempt to delete in system namespace " << ns << endl;
			return -1;
		}
	}

	int nDeleted = 0;
	JSMatcher matcher(pattern);
	JSObj order;
	auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
	if( c.get() == 0 )
		c = theDataFileMgr.findAll(ns);

	Cursor &tempDebug = *c;

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
			assert( !deep || !c->getsetdup(rloc) ); // can't be a dup, we deleted it!
			if( !justOne )
				c->noteLocation();

			theDataFileMgr.deleteRecord(ns, r, rloc);
			nDeleted++;
			if( justOne )
				break;
			c->checkLocation();
		}
	}

	return nDeleted;
}

struct Mod { 
	enum Op { INC, SET } op;
	const char *fieldName;
	double *n;
	static void getMods(vector<Mod>& mods, JSObj from);
	static void applyMods(vector<Mod>& mods, JSObj obj);
};

void Mod::applyMods(vector<Mod>& mods, JSObj obj) { 
	for( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ ) { 
		Mod& m = *i;
		Element e = obj.findElement(m.fieldName);
		if( e.type() == Number ) {
			if( m.op == INC )
				*m.n = e.number() += *m.n;
			else
				e.number() = *m.n; // $set or $SET
		}
	}
}

/* get special operations like $inc 
   { $inc: { a:1, b:1 } }
   { $set: { a:77 } }
   NOTE: MODIFIES source from object!
*/
void Mod::getMods(vector<Mod>& mods, JSObj from) { 
	JSElemIter it(from);
	while( it.more() ) {
		Element e = it.next();
		const char *fn = e.fieldName();
		if( *fn == '$' && e.type() == Object && 
			fn[4] == 0 ) {
			JSObj j = e.embeddedObject();
			JSElemIter jt(j);
			Op op = Mod::SET;
			if( strcmp("$inc",fn) == 0 ) {
				op = Mod::INC;
				// we rename to $SET instead of $set so that on an op like
				//   { $set: {x:1}, $inc: {y:1} }
				// we don't get two "$set" fields which isn't allowed
				strcpy((char *) fn, "$SET");
			}
			while( jt.more() ) { 
				Element f = jt.next();
				if( f.eoo() )
					break;
				Mod m;
				m.op = op;
				m.fieldName = f.fieldName();
				if( f.type() == Number ) {
					m.n = &f.number();
					mods.push_back( m );
				} 
			}
		}
	}
}

/* todo:
     _ smart requery find record immediately
   returns:  
     2: we did applyMods() but didn't logOp()
	 5: we did applyMods() and did logOp() (so don't do it again) 
     (clean these up later...)
*/
int _updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert, stringstream& ss, bool logop=false) {
	//cout << "TEMP BAD";
	//lrutest.find(updateobj);

	int profile = client->profile;

	//	cout << "update ns:" << ns << " objsize:" << updateobj.objsize() << " queryobjsize:" << 
	//		pattern.objsize();

	if( strstr(ns, ".system.") ) { 
		cout << "\nERROR: attempt to update in system namespace " << ns << endl;
		ss << " can't update system namespace ";
		return 0;
	}

	int nscanned = 0;
	{
		JSMatcher matcher(pattern);
		JSObj order;
		auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
		if( c.get() == 0 )
			c = theDataFileMgr.findAll(ns);
		while( c->ok() ) {
			Record *r = c->_current();
			nscanned++;
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

				if( profile )
					ss << " nscanned:" << nscanned;

				/* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
				   regular ones at the moment. */
				const char *firstField = updateobj.firstElement().fieldName();
				if( firstField[0] == '$' ) {
					vector<Mod> mods;
					Mod::getMods(mods, updateobj);
					Mod::applyMods(mods, c->currLoc().obj());
					if( profile ) 
						ss << " fastmod ";
					if( logop ) {
						if( mods.size() ) { 
							logOp("u", ns, updateobj, &pattern, &upsert);
							return 5;
						}
					}
					return 2;
				}

				theDataFileMgr.update(ns, r, c->currLoc(), updateobj.objdata(), updateobj.objsize(), ss);
				return 1;
			}
			c->advance();
		}
	}

	if( profile )
		ss << " nscanned:" << nscanned;

	if( upsert ) {
		if( updateobj.firstElement().fieldName()[0] == '$' ) {
			/* upsert of an $inc. build a default */
			vector<Mod> mods;
			Mod::getMods(mods, updateobj);
			JSObjBuilder b;
			b.appendElements(pattern);
			for( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ )
				b.append(i->fieldName, *i->n);
			JSObj obj = b.done();
			theDataFileMgr.insert(ns, (void*) obj.objdata(), obj.objsize());
			if( profile )
				ss << " fastmodinsert ";
			return 3;
		}
		if( profile )
			ss << " upsert ";
		theDataFileMgr.insert(ns, (void*) updateobj.objdata(), updateobj.objsize());
		return 4;
	}
	return 0;
}
/* todo: we can optimize replication by just doing insert when an upsert triggers. 
*/
void updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert, stringstream& ss) {
	int rc = _updateObjects(ns, updateobj, pattern, upsert, ss, true);
	if( rc != 5 )
		logOp("u", ns, updateobj, &pattern, &upsert);
}

int queryTraceLevel = 0;
int otherTraceLevel = 0;

int initialExtentSize(int len);

void clean(const char *ns, NamespaceDetails *d) {
	for( int i = 0; i < Buckets; i++ ) 
		d->deletedList[i].Null();
}

string validateNS(const char *ns, NamespaceDetails *d) {
	bool valid = true;
	stringstream ss;
	ss << "\nvalidate\n";
	ss << "  details: " << hex << d << " ofs:" << nsindex(ns)->detailsOffset(d) << dec << endl;
	if( d->capped )
		ss << "  capped:" << d->capped << " max:" << d->max << '\n';

	ss << "  firstExtent:" << d->firstExtent.toString() << " ns:" << d->firstExtent.ext()->ns.buf << '\n';
	ss << "  lastExtent:" << d->lastExtent.toString()    << " ns:" << d->lastExtent.ext()->ns.buf << '\n';
	try { 
		d->firstExtent.ext()->assertOk();
		d->lastExtent.ext()->assertOk(); 
	} catch(...) { valid=false; ss << " extent asserted "; }

	ss << "  datasize?:" << d->datasize << " nrecords?:" << d->nrecords << " lastExtentSize:" << d->lastExtentSize << '\n';
	ss << "  padding:" << d->paddingFactor << '\n';
	try { 

		try {
			ss << "  first extent:\n";
			d->firstExtent.ext()->dump(ss);
			valid = valid && d->firstExtent.ext()->validates();
		}
		catch(...) { 
		  ss << "\n    exception firstextent\n" << endl;
		}

		auto_ptr<Cursor> c = theDataFileMgr.findAll(ns);
		int n = 0;
		long long len = 0;
		long long nlen = 0;
		set<DiskLoc> recs;
		int outOfOrder = 0;
		DiskLoc cl_last;
		while( c->ok() ) { 
			n++;

			DiskLoc cl = c->currLoc();
			if( n < 1000000 )
				recs.insert(cl);
			if( d->capped ) {
				if( cl < cl_last )
					outOfOrder++;
				cl_last = cl;
			}

			Record *r = c->_current();
			len += r->lengthWithHeaders;
			nlen += r->netLength();
			c->advance();
		}
		if( d->capped ) { 
			ss << "  capped outOfOrder:" << outOfOrder;
			if( outOfOrder > 1 ) { 
				valid = false;
				ss << " ???";
			}
			else ss << " (OK)";
			ss << '\n';
		}
		ss << "  " << n << " objects found, nobj:" << d->nrecords << "\n";
		ss << "  " << len << " bytes data w/headers\n";
		ss << "  " << nlen << " bytes data wout/headers\n";

		ss << "  deletedList: ";
		for( int i = 0; i < Buckets; i++ ) { 
			ss << (d->deletedList[i].isNull() ? '0' : '1');
		}
		ss << endl;
		int ndel = 0;
		long long delSize = 0;
		int incorrect = 0;
		for( int i = 0; i < Buckets; i++ ) { 
			DiskLoc loc = d->deletedList[i];
			try {
			  int k = 0;
			  while( !loc.isNull() ) { 
			    if( recs.count(loc) )
			      incorrect++;
			    ndel++;

			    if( loc.questionable() ) { 
					if( loc.a() <= 0 || strstr(ns, "hudsonSmall") == 0 ) {
						ss << "    ?bad deleted loc: " << loc.toString() << " bucket:" << i << " k:" << k << endl;
						valid = false;
						break;
					}
			    }

			    DeletedRecord *d = loc.drec();
			    delSize += d->lengthWithHeaders;
			    loc = d->nextDeleted;
			    k++;
			  }
			} catch(...) { ss <<"    ?exception in deleted chain for bucket " << i << endl; valid = false; }
		}
		ss << "  deleted: n: " << ndel << " size: " << delSize << '\n';
		if( incorrect ) { 
			ss << "    ?corrupt: " << incorrect << " records from datafile are in deleted list\n";
			valid = false;
		}

		int idxn = 0;
		try  {
			ss << "  nIndexes:" << d->nIndexes << endl;
			for( ; idxn < d->nIndexes; idxn++ ) {
				ss << "    " << d->indexes[idxn].indexNamespace() << " keys:" << 
					d->indexes[idxn].head.btree()->fullValidate(d->indexes[idxn].head) << endl;
			}
		} 
		catch(...) { 
		  ss << "\n    exception during index validate idxn:" << idxn << endl; valid=false;
		}

	}
	catch(AssertionException) {
		ss << "\n    exception during validate\n" << endl; 
		valid = false;
	}

	if( !valid ) 
		ss << " ns corrupt, requires dbchk\n";

	return ss.str();
}

bool userCreateNS(const char *ns, JSObj& j, string& err);

const int edebug=0;

bool dbEval(JSObj& cmd, JSObjBuilder& result) { 
	Element e = cmd.firstElement();
	assert( e.type() == Code );
	const char *code = e.valuestr();
	if ( ! JavaJS ) {
		result.append("errmsg", "db side execution is disabled");
		return false;
	}

	jlong f = JavaJS->functionCreate(code);
	if( f == 0 ) { 
		result.append("errmsg", "compile failed");
		return false;
	}

	Scope s;
	s.setString("$client", client->name.c_str());
	Element args = cmd.findElement("args");
	if( args.type() == Array ) {
		JSObj eo = args.embeddedObject();
		if( edebug ) {
			cout << "args:" << eo.toString() << endl;
			cout << "code:\n" << code << endl;
		}
		s.setObject("args", eo);
	}

	int res = s.invoke(f);
	if( res ) {
		result.append("errno", (double) res);
		result.append("errmsg", "invoke failed");
		return false;
	}

	int type = s.type("return");
	if( type == Object || type == Array )
		result.append("retval", s.getObject("return"));
	else if( type == Number ) 
		result.append("retval", s.getNumber("return"));
	else if( type == String )
		result.append("retval", s.getString("return").c_str());
	else if( type == Bool ) {
		result.appendBool("retval", s.getBoolean("return"));
	}

	return true;
}

extern int opLogging;
void flushOpLog();

// e.g.
//   system.cmd$.find( { queryTraceLevel: 2 } );
// 
// returns true if ran a cmd
//
bool _runCommands(const char *ns, JSObj& jsobj, stringstream& ss, BufBuilder &b, JSObjBuilder& anObjBuilder) { 

	const char *p = strchr(ns, '.');
	if( !p ) return false;
	if( strcmp(p, ".$cmd") != 0 ) return false;

	bool ok = false;
	bool valid = false;

	//cout << jsobj.toString() << endl;

	Element e;
	e = jsobj.firstElement();

	if( e.eoo() ) goto done;
	if( e.type() == Code ) { 
		valid = true;
		ok = dbEval(jsobj, anObjBuilder);
	}
	else if( e.type() == Number ) { 
		if( strcmp(e.fieldName(), "getoptime") == 0 ) { 
			valid = true;
			ok = true;
			anObjBuilder.appendDate("optime", OpTime::now().asDate());
		}
		else if( strcmp(e.fieldName(), "dropDatabase") == 0 ) { 
			if( 1 ) {
				cout << "dropDatabase " << ns << endl;
				valid = true;
				int p = (int) e.number();
				if( p != 1 ) {
					ok = false;
				} else { 
					dropDatabase(ns);
					ok = true;
				}
			}
			else { 
				cout << "TEMP CODE: dropdatabase commented out " << endl;
			}
		}
		else if( strcmp(e.fieldName(), "profile") == 0 ) { 
			anObjBuilder.append("was", (double) client->profile);
			int p = (int) e.number();
			valid = true;
			if( p == -1 )
				ok = true;
			else if( p >= 0 && p <= 2 ) { 
				ok = true;
				client->profile = p;
			}
			else {
				ok = false;
			}
		}
		else {
			// admin only commands.
			if( strncmp(ns, "admin", p-ns) != 0 ) 
				return false;
			if( strcmp(e.fieldName(),"opLogging") == 0 ) {
				valid = ok = true;
				opLogging = (int) e.number();
				flushOpLog();
				log() << "CMD: opLogging set to " << opLogging << endl;
			} else if( strcmp(e.fieldName(),"queryTraceLevel") == 0 ) {
				valid = ok = true;
				queryTraceLevel = (int) e.number();
			} else if( strcmp(e.fieldName(),"traceAll") == 0 ) { 
				valid = ok = true;
				queryTraceLevel = (int) e.number();
				otherTraceLevel = (int) e.number();
			}
		}
	}
	else if( e.type() == String ) {
		/* { count: "collectionname"[, query: <query>] } */
		string us(ns, p-ns);

		if( strcmp( e.fieldName(), "count" ) == 0 ) { 
			valid = true;
			string ns = us + '.' + e.valuestr();
			string err;
			int n = runCount(ns.c_str(), jsobj, err);
			int nn = n;
			ok = true;
			if( n < 0 ) { 
				ok = false;
				nn = 0;
				if( !err.empty() )
					anObjBuilder.append("errmsg", err.c_str());
			}
			anObjBuilder.append("n", (double) nn);
		}
		else if( strcmp( e.fieldName(), "clone") == 0 ) { 
			valid = true;
			string err;
			ok = cloneFrom(e.valuestr(), err);
			if( !err.empty() ) 
				anObjBuilder.append("errmsg", err.c_str());
		}
		else if( strcmp( e.fieldName(), "create") == 0 ) { 
			valid = true;
			string ns = us + '.' + e.valuestr();
			string err;
			ok = userCreateNS(ns.c_str(), jsobj, err);
			if( ok )
				logOp("c", ns.c_str(), jsobj);
			if( !ok && !err.empty() )
				anObjBuilder.append("errmsg", err.c_str());
		}
		else if( strcmp( e.fieldName(), "clean") == 0 ) { 
			valid = true;
			string dropNs = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(dropNs.c_str());
			log() << "CMD: clean " << dropNs << endl;
			if( d ) { 
				ok = true;
				anObjBuilder.append("ns", dropNs.c_str());
				clean(dropNs.c_str(), d);
			}
			else {
				anObjBuilder.append("errmsg", "ns not found");
			}
		}
		else if( strcmp( e.fieldName(), "drop") == 0 ) { 
			valid = true;
			string nsToDrop = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(nsToDrop.c_str());
			log() << "CMD: drop " << nsToDrop << endl;
			if( d == 0 ) {
				anObjBuilder.append("errmsg", "ns not found");
			}
			else if( d->nIndexes != 0 ) {
				// client is supposed to drop the indexes first
				anObjBuilder.append("errmsg", "ns has indexes (not permitted on drop)");
			}
			else {
				ok = true;
				anObjBuilder.append("ns", nsToDrop.c_str());
				ClientCursor::invalidate(nsToDrop.c_str());
				dropNS(nsToDrop);
				logOp("c", ns, jsobj);
				/*
				{
					JSObjBuilder b;
					b.append("name", dropNs.c_str());
					JSObj cond = b.done(); // { name: "colltodropname" }
					deleteObjects("system.namespaces", cond, false, true);
				}
				client->namespaceIndex.kill(dropNs.c_str());
				*/
			}
		}
		else if( strcmp( e.fieldName(), "validate") == 0 ) { 
			valid = true;
			string toValidateNs = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(toValidateNs.c_str());
			log() << "CMD: validate " << toValidateNs << endl;
			if( d ) { 
				ok = true;
				anObjBuilder.append("ns", toValidateNs.c_str());
				string s = validateNS(toValidateNs.c_str(), d);
				anObjBuilder.append("result", s.c_str());
			}
			else {
				anObjBuilder.append("errmsg", "ns not found");
			}
		}
		else if( strcmp(e.fieldName(),"deleteIndexes") == 0 ) { 
			valid = true;
			/* note: temp implementation.  space not reclaimed! */
			string toDeleteNs = us + '.' + e.valuestr();
			NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
			log() << "CMD: deleteIndexes " << toDeleteNs << endl;
			if( d ) {
				Element f = jsobj.findElement("index");
				if( !f.eoo() ) { 

					d->aboutToDeleteAnIndex();

					ClientCursor::invalidate(toDeleteNs.c_str());
					logOp("c", ns, jsobj);

					// delete a specific index or all?
					if( f.type() == String ) { 
						const char *idxName = f.valuestr();
						if( *idxName == '*' && idxName[1] == 0 ) { 
							ok = true;
							cout << "  d->nIndexes was " << d->nIndexes << endl;
							anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
							anObjBuilder.append("msg", "all indexes deleted for collection");
							cout << "  alpha implementation, space not reclaimed" << endl;
							for( int i = 0; i < d->nIndexes; i++ )
								d->indexes[i].kill();
							d->nIndexes = 0;
						}
						else {
							// delete just one index
							int x = d->findIndexByName(idxName);
							if( x >= 0 ) { 
								cout << "  d->nIndexes was " << d->nIndexes << endl;
								anObjBuilder.append("nIndexesWas", (double)d->nIndexes);

								/* note it is  important we remove the IndexDetails with this 
								   call, otherwise, on recreate, the old one would be reused, and its
								   IndexDetails::info ptr would be bad info.
								*/
								d->indexes[x].kill();

								d->nIndexes--;
								for( int i = x; i < d->nIndexes; i++ )
									d->indexes[i] = d->indexes[i+1];
								ok=true;
								cout << "  alpha implementation, space not reclaimed\n";
							} else { 
								cout << "deleteIndexes: " << idxName << " not found" << endl;
							}
						}
					}
				}
			}
			else {
				anObjBuilder.append("errmsg", "ns not found");
			}
		}
	}

done:
	if( !valid )
		anObjBuilder.append("errmsg", "no such cmd");
	anObjBuilder.append("ok", ok?1.0:0.0);
	JSObj x = anObjBuilder.done();
	b.append((void*) x.objdata(), x.objsize());
	return true;
}

bool runCommands(const char *ns, JSObj& jsobj, stringstream& ss, BufBuilder &b, JSObjBuilder& anObjBuilder) { 
	try {
		return _runCommands(ns, jsobj, ss, b, anObjBuilder);
	}
	catch( AssertionException ) {
		;
	}
	ss << " assertion ";
	anObjBuilder.append("errmsg", "db assertion failure");
	anObjBuilder.append("ok", 0.0);
	JSObj x = anObjBuilder.done();
	b.append((void*) x.objdata(), x.objsize());
	return true;
}

int nCaught = 0;

void killCursors(int n, long long *ids) {
	int k = 0;
	for( int i = 0; i < n; i++ ) {
		if( ClientCursor::erase(ids[i]) )
			k++;
	}
	log() << "killCursors: found " << k << " of " << n << '\n';
}

// order.$natural sets natural order direction
auto_ptr<Cursor> findTableScan(const char *ns, JSObj& order);

JSObj id_obj = fromjson("{_id:ObjId()}");
JSObj empty_obj = fromjson("{}");

/* { count: "collectionname"[, query: <query>] } 
   returns -1 on error.
*/
int runCount(const char *ns, JSObj& cmd, string& err) { 
	NamespaceDetails *d = nsdetails(ns);
	if( d == 0 ) {
		err = "ns does not exist";
		return -1;
	}

	JSObj query = cmd.getObjectField("query");

	if( query.isEmpty() ) { 
		// count of all objects
		return (int) d->nrecords;
	}

	auto_ptr<Cursor> c;

	bool simpleKeyToMatch = false;
	c = getIndexCursor(ns, query, empty_obj, &simpleKeyToMatch);

	if( c.get() ) {
		if( simpleKeyToMatch ) { 
			/* Here we only look at the btree keys to determine if a match, instead of looking 
			   into the records, which would be much slower.
			   */
			int count = 0;
			BtreeCursor *bc = dynamic_cast<BtreeCursor *>(c.get());
			if( c->ok() ) {
				while( 1 ) {
					if( !(query == bc->currKeyNode().key) )
						break;
					count++;
					if( !c->advance() )
						break;
				}
			}
			return count;
		}
	} else {
		c = findTableScan(ns, empty_obj);
	}

	int count = 0;
	auto_ptr<JSMatcher> matcher(new JSMatcher(query));
	while( c->ok() ) {
		JSObj js = c->current();
		bool deep;
		if( !matcher->matches(js, &deep) ) {
			if( c->tempStopOnMiss() )
				break;
		}
		else if( !deep || !c->getsetdup(c->currLoc()) ) { // i.e., check for dups on deep items only
			// got a match.
			count++;
		}
		c->advance();
	}
	return count;
}

QueryResult* runQuery(Message& message, const char *ns, int ntoskip, int _ntoreturn, JSObj jsobj, 
					  auto_ptr< set<string> > filter, stringstream& ss, int queryOptions) 
{
	time_t t = time(0);
	bool wantMore = true;
	int ntoreturn = _ntoreturn;
	if( _ntoreturn < 0 ) {
		ntoreturn = -_ntoreturn;
		wantMore = false;
	}
	ss << "query " << ns << " ntoreturn:" << ntoreturn;

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

		/* The ElemIter will not be happy if this isn't really an object. So throw exception
		   here when that is true.
 		   (Which may indicate bad data from appserver?)
		*/
		if( query.objsize() == 0 ) { 
			cout << "Bad query object?\n  jsobj:";
			cout << jsobj.toString() << "\n  query:";
			cout << query.toString() << endl;
			assert(false);
		}

		auto_ptr<JSMatcher> matcher(new JSMatcher(query));
		JSMatcher &debug1 = *matcher;
		assert( debug1.getN() < 5000 );

		bool isSorted = false;
		int nscanned = 0;
		auto_ptr<Cursor> c = getSpecialCursor(ns);
		if( c.get() == 0 )
			c = getIndexCursor(ns, query, order, 0, &isSorted);
		if( c.get() == 0 )
			c = findTableScan(ns, order);

		auto_ptr<ScanAndOrder> so;
		bool ordering = false;
		if( !order.isEmpty() && !isSorted ) {
			ordering = true;
			ss << " scanAndOrder ";
			so = auto_ptr<ScanAndOrder>(new ScanAndOrder());
			wantMore = false;
			//			scanAndOrder(b, c.get(), order, ntoreturn);
		}

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
			else if( !deep || !c->getsetdup(c->currLoc()) ) { // i.e., check for dups on deep items only
				// got a match.
				if( ntoskip > 0 ) {
					ntoskip--;
				}
				else {
					bool ok = true;
					assert( js.objsize() >= 0 ); //defensive for segfaults
					/*if( ordering )
						so->add(js);
					else*/ if( filter.get() ) {
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
						if( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
							(ntoreturn==0 && (b.len()>1*1024*1024 || n>=101)) ) {
								/* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there 
								   is only a size limit.  The idea is that on a find() where one doesn't use much results, 
								   we don't return much, but once getmore kicks in, we start pushing significant quantities.

								   The n limit (vs. size) is important when someone fetches only one small field from big 
								   objects, which causes massive scanning server-side.
								*/
								/* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
								if( wantMore && ntoreturn != 1 ) {
									if( useCursors ) {
										c->advance();
										if( c->ok() ) {
											// more...so save a cursor
											ClientCursor *cc = new ClientCursor();
											cc->c = c;
											cursorid = cc->cursorid;
											DEV cout << "  query has more, cursorid: " << cursorid << endl;
											cc->matcher = matcher;
											cc->ns = ns;
											cc->pos = n;
											cc->filter = filter;
											cc->originalMessage = message;
											cc->updateLocation();
										}
									}
								}
								break;
						}
					}
				}
			}
			c->advance();
		}

		if( cursorid == 0 && (queryOptions & Option_CursorTailable) && c->tailable() ) { 
			c->setAtTail();
			ClientCursor *cc = new ClientCursor();
			cc->c = c;
			cursorid = cc->cursorid;
			DEV cout << "  query has no more but tailable, cursorid: " << cursorid << endl;
			cc->matcher = matcher;
			cc->ns = ns;
			cc->pos = n;
			cc->filter = filter;
			cc->originalMessage = message;
			cc->updateLocation();
		}

		if( client->profile )
			ss << "  nscanned:" << nscanned << ' ';
	}

	QueryResult *qr = (QueryResult *) b.buf();
	qr->_data[0] = 0;
	qr->_data[1] = 0;
	qr->_data[2] = 0;
	qr->_data[3] = 0;
	qr->len = b.len();
	ss << " reslen:" << b.len();
	//	qr->channel = 0;
	qr->operation = opReply;
	qr->cursorId = cursorid;
	qr->startingFrom = 0;
	qr->nReturned = n;
	b.decouple();

	if( (client && client->profile) || time(0)-t > 5 ) {
		if( ntoskip ) 
			ss << " ntoskip:" << ntoskip;
		ss << " <br>query: " << jsobj.toString() << ' ';
	}
	ss << " nreturned:" << n;
	return qr;
}

//int dump = 0;

/* empty result for error conditions */
QueryResult* emptyMoreResult(long long cursorid) {
	BufBuilder b(32768);
	b.skip(sizeof(QueryResult));
	QueryResult *qr = (QueryResult *) b.buf();
	qr->cursorId = 0; // 0 indicates no more data to retrieve.
	qr->startingFrom = 0;
	qr->len = b.len();
	qr->operation = opReply;
	qr->nReturned = 0;
	b.decouple();
	return qr;
}

QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid) {
	BufBuilder b(32768);

	ClientCursor *cc = ClientCursor::find(cursorid);

	b.skip(sizeof(QueryResult));

	int resultFlags = 0;
	int start = 0;
	int n = 0;

	if( !cc ) { 
		DEV log() << "getMore: cursorid not found " << ns << " " << cursorid << endl;
		cursorid = 0;
		resultFlags = ResultFlag_CursorNotFound;
	}
	else {
		start = cc->pos;
		Cursor *c = cc->c.get();
		c->checkLocation();
		c->tailResume();
		while( 1 ) {
			if( !c->ok() ) {
done:
				if( c->tailing() ) {
					c->setAtTail();
					break;
				}
				DEV log() << "  getmore: last batch, erasing cursor " << cursorid << endl;
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
			else { 
				//cout << "matches " << c->currLoc().toString() << ' ' << deep << '\n';
				if( deep && c->getsetdup(c->currLoc()) ) { 
					//cout << "  but it's a dup \n";
				}
				else {
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
						if( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
							(ntoreturn==0 && b.len()>1*1024*1024) ) {
								c->advance();
								if( c->tailing() && !c->ok() )
									c->setAtTail();
								cc->pos += n;
								//cc->updateLocation();
								break;
						}
					}
				}
			}
			c->advance();
		}
        if( cc ) 
            cc->updateLocation();
	}

	QueryResult *qr = (QueryResult *) b.buf();
	qr->len = b.len();
	qr->operation = opReply;
	qr->resultFlags() = resultFlags;
	qr->cursorId = cursorid;
	qr->startingFrom = start;
	qr->nReturned = n;
	b.decouple();

	return qr;
}
