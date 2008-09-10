/* commands.cpp
   db "commands" (sent via db.$cmd.findOne(...))
 */

/**
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
#include "commands.h"

extern int queryTraceLevel;
extern int otherTraceLevel;
extern int opLogging;
bool userCreateNS(const char *ns, JSObj& j, string& err);
void flushOpLog();
int runCount(const char *ns, JSObj& cmd, string& err);

const int edebug=0;

map<string,Command*> Command::commands;

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

class CmdGetOpTime : public Command { 
public:
    CmdGetOpTime() : Command("getoptime") { }
    bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) {
        result.appendDate("optime", OpTime::now().asDate());
        return true;
    }
} cmdgetoptime;

/*
class Cmd : public Command { 
public:
    Cmd() : Command("") { }
    bool adminOnly() { return true; }
    bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) {
        return true;
    }
} cmd;
*/

class CmdOpLogging : public Command { 
public:
    CmdOpLogging() : Command("opLogging") { }
    bool adminOnly() { return true; }
    bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) {
        opLogging = (int) cmdObj.findElement("opLogging").number();
        flushOpLog();
        log() << "CMD: opLogging set to " << opLogging << endl;
        return true;
    }
} cmdoplogging;

class CmdQueryTraceLevel : public Command { 
public:
    CmdQueryTraceLevel() : Command("queryTraceLevel") { }
    bool adminOnly() { return true; }
    bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) {
        queryTraceLevel = (int) cmdObj.findElement(name.c_str()).number();
        return true;
    }
} cmdquerytracelevel;

class Cmd : public Command { 
public:
    Cmd() : Command("traceAll") { }
    bool adminOnly() { return true; }
    bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) {
        queryTraceLevel = otherTraceLevel = (int) cmdObj.findElement(name.c_str()).number();
        return true;
    }
} cmdtraceall;

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

    map<string,Command*>::iterator i;

	if( e.eoo() ) 
        ;
    /* check for properly registered command objects.  Note that all the commands below should be 
       migrated over to the command object format.
       */
    else if( (i = Command::commands.find(e.fieldName())) != Command::commands.end() ) { 
        valid = true;
        string errmsg;
        Command *c = i->second;
        if( c->adminOnly() && strncmp(ns, "admin", p-ns) != 0 ) {
            ok = false;
			errmsg = "access denied";
        }
        else {
            ok = c->run(ns, jsobj, errmsg, anObjBuilder);
        }
        if( !ok ) 
            anObjBuilder.append("errmsg", errmsg);
    }
	else if( e.type() == Code ) { 
		valid = true;
		ok = dbEval(jsobj, anObjBuilder);
	}
	else if( e.type() == Number ) { 
        if( strcmp(e.fieldName(), "dropDatabase") == 0 ) { 
			if( 1 ) {
				cout << "dropDatabase " << ns << endl;
				valid = true;
				int p = (int) e.number();
				if( p != 1 ) {
					ok = false;
				} else { 
					dropDatabase(ns);
					logOp("c", ns, jsobj);
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
							log() << "  d->nIndexes was " << d->nIndexes << '\n';
							anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
							anObjBuilder.append("msg", "all indexes deleted for collection");
							for( int i = 0; i < d->nIndexes; i++ )
								d->indexes[i].kill();
							d->nIndexes = 0;
							log() << "  alpha implementation, space not reclaimed" << endl;
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

	if( !valid )
		anObjBuilder.append("errmsg", "no such cmd");
	anObjBuilder.append("ok", ok?1.0:0.0);
	JSObj x = anObjBuilder.done();
	b.append((void*) x.objdata(), x.objsize());
	return true;
}

