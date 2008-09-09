// matcher.cpp

/* JSMatcher is our boolean expression evaluator for "where" clauses */

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
#include "jsobj.h"
#include "../util/goodies.h"
#include "javajs.h"
#include "../util/unittest.h"

#if defined(_WIN32)

#include <hash_map>
using namespace stdext;

typedef const char * MyStr;
struct less_str {
   bool operator()(const MyStr & x, const MyStr & y) const {
      if ( strcmp(x, y) > 0)
         return true;

      return false;
   }
};

typedef hash_map<const char*, int, hash_compare<const char *, less_str> > strhashmap;

#else

#include <ext/hash_map>
using namespace __gnu_cxx;

typedef const char * MyStr;
struct eq_str {
   bool operator()(const MyStr & x, const MyStr & y) const {
      if ( strcmp(x, y) == 0)
         return true;

      return false;
   }
};

typedef hash_map<const char*, int, hash<const char *>, eq_str > strhashmap;

#endif

#include "minilex.h"

MiniLex minilex;

class Where { 
public:
	Where() { codeCopy = 0; }
	~Where() {
		JavaJS->scopeFree(scope);
		delete codeCopy;
		scope = 0; func = 0; codeCopy = 0;
	}
	jlong scope, func;
	strhashmap fields;
//	map<string,int> fields;
	bool fullObject;
	int nFields;
	char *codeCopy;

	void setFunc(const char *code) {
		codeCopy = new char[strlen(code)+1];
		strcpy(codeCopy,code);
		func = JavaJS->functionCreate( code );
		minilex.grabVariables(codeCopy, fields);
		// if user references db, eg db.foo.save(obj), 
		// we make sure we have the whole thing.
		fullObject = fields.count("fullObject") +
			fields.count("db") > 0;
		nFields = fields.size();
	}

	void buildSubset(JSObj& src, JSObjBuilder& dst) { 
		JSElemIter it(src);
		int n = 0;
		if( !it.more() ) return;
		while( 1 ) {
			Element e = it.next();
			if( e.eoo() )
				break;
			if( //n == 0 && 
				fields.find(e.fieldName()) != fields.end()
				//fields.count(e.fieldName())
				) {
				dst.append(e);
				if( ++n >= nFields )
					break;
			}
		}
	}
};

JSMatcher::~JSMatcher() { 
	for( int i = 0; i < nBuilders; i++ )
		delete builders[i];
	delete in;
	delete where;
}

#include "pdfile.h"

JSMatcher::JSMatcher(JSObj &_jsobj) : 
   in(0), where(0), jsobj(_jsobj), nRegex(0)
{
	nBuilders = 0;

	JSElemIter i(jsobj);
	n = 0;
	while( i.more() ) {
		Element e = i.next();
		if( e.eoo() )
			break;

		if( e.type() == Code && strcmp(e.fieldName(), "$where")==0 ) { 
			// $where: function()...
			assert( where == 0 );
			where = new Where();
			const char *code = e.valuestr();
			massert( "$where query, but jni is disabled", JavaJS );
			where->scope = JavaJS->scopeCreate();
			JavaJS->scopeSetString(where->scope, "$client", client->name.c_str());
			where->setFunc(code);
			continue;
		}

		if( e.type() == RegEx ) {
			if( nRegex >= 4 ) {
				cout << "ERROR: too many regexes in query" << endl;
			}
			else {
				pcrecpp::RE_Options options;
				options.set_utf8(true);
				const char *flags = e.regexFlags();
				while( flags && *flags ) { 
					if( *flags == 'i' )
						options.set_caseless(true);
					else if( *flags == 'm' )
						options.set_multiline(true);
					else if( *flags == 'x' )
						options.set_extended(true);
					flags++;
				}
				regexs[nRegex].re = new pcrecpp::RE(e.regex(), options);
				regexs[nRegex].fieldName = e.fieldName();
				nRegex++;
			}
			continue;
		}

		// greater than / less than...
		// e.g., e == { a : { $gt : 3 } }
		//       or 
		//            { a : { $in : [1,2,3] } }
		if( e.type() == Object ) {
			// e.g., fe == { $gt : 3 }
			JSElemIter j(e.embeddedObject());
			bool ok = false;
			while( j.more() ) {
				Element fe = j.next();
				if( fe.eoo() ) 
					break;
				// Element fe = e.embeddedObject().firstElement();
				const char *fn = fe.fieldName();
				if( fn[0] == '$' && fn[1] ) { 
					if( fn[2] == 't' ) { 
						int op = Equality;
						if( fn[1] == 'g' ) { 
							if( fn[3] == 0 ) op = GT;
							else if( fn[3] == 'e' && fn[4] == 0 ) op = GTE;
						}
						else if( fn[1] == 'l' ) { 
							if( fn[3] == 0 ) op = LT;
							else if( fn[3] == 'e' && fn[4] == 0 ) op = LTE;
						}
						if( op && nBuilders < 8) { 
							JSObjBuilder *b = new JSObjBuilder();
							builders[nBuilders++] = b;
							b->appendAs(fe, e.fieldName());
							toMatch.push_back( b->done().firstElement() );
							compareOp.push_back(op);
							n++;
							ok = true;
						}
					}
					else if( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0 && fe.type() == Array ) {
						// $in
						assert( in == 0 ); // only one per query supported so far.  finish...
						in = new set<Element,element_lt>();
						JSElemIter i(fe.embeddedObject());
                        if( i.more() ) {
                            while( 1 ) {
                                Element ie = i.next();
                                if( ie.eoo() ) 
                                    break;
                                in->insert(ie);
                            }
                        }
						toMatch.push_back(e); // not actually used at the moment
						compareOp.push_back(opIN);
						n++;
						ok = true;
					}
				}
				else {
					ok = false;
					break;
				}
			}
			if( ok ) 
				continue;
		}

		{
			// normal, simple case e.g. { a : "foo" }
			toMatch.push_back(e);
			compareOp.push_back(Equality);
			n++;
		}
	}
}

inline int JSMatcher::valuesMatch(Element& l, Element& r, int op) { 
	if( op == 0 ) 
		return l.valuesEqual(r);

	if( op == opIN ) {
		// { $in : [1,2,3] }
        int c = in->count(l);
        return c;
	}

	/* check LT, GTE, ... */
	if( l.type() != r.type() )
		return false;
	int c = compareElementValues(l, r);
	int z = 1 << (c+1); 
	return (op & z);
}

/* Check if a particular field matches.

   fieldName - field to match "a.b" if we are reaching into an embedded object.
   toMatch   - element we want to match.
   obj       - database object to check against
   compareOp - Equality, LT, GT, etc.
   deep      - out param.  set to true/false if we scanned an array
   isArr     - 

   Special forms:

     { "a.b" : 3 }             means       obj.a.b == 3
     { a : { $lt : 3 } }       means       obj.a < 3
	 { a : { $in : [1,2] } }   means       [1,2].contains(obj.a)

   return value
   -1 mismatch
    0 missing element
    1 match
*/
int JSMatcher::matchesDotted(const char *fieldName, Element& toMatch, JSObj& obj, int compareOp, bool *deep, bool isArr) { 
	{
		const char *p = strchr(fieldName, '.');
		if( p ) { 
			string left(fieldName, p-fieldName);

			Element e = obj.getField(left.c_str());
			if( e.eoo() )
				return 0;
			if( e.type() != Object && e.type() != Array )
				return -1;

			JSObj eo = e.embeddedObject();
			return matchesDotted(p+1, toMatch, eo, compareOp, deep, e.type() == Array);
		}
	}

	Element e = obj.getField(fieldName);

	if( valuesMatch(e, toMatch, compareOp) ) {
		return 1;
	}
	else if( e.type() == Array ) {
		JSElemIter ai(e.embeddedObject());
		while( ai.more() ) { 
			Element z = ai.next();
			if( valuesMatch( z, toMatch, compareOp) ) {
				if( deep )
					*deep = true;
				return 1;
			}
		}
	}
	else if( isArr ) { 
		JSElemIter ai(obj);
		while( ai.more() ) { 
			Element z = ai.next();
			if( z.type() == Object ) {
				JSObj eo = z.embeddedObject();
				int cmp = matchesDotted(fieldName, toMatch, eo, compareOp, deep);
				if( cmp > 0 ) { 
					if( deep ) *deep = true;
					return 1;
				}
			}
		}
	}
	else if( e.eoo() ) { 
		return 0;
	}
	return -1;
}

extern int dump;

inline bool _regexMatches(RegexMatcher& rm, Element& e) { 
	char buf[64];
	const char *p = buf;
	if( e.type() == String || e.type() == Symbol )
		p = e.valuestr();
	else if( e.type() == Number ) { 
		sprintf(buf, "%f", e.number());
	}
	else if( e.type() == Date ) { 
		unsigned long long d = e.date();
		time_t t = (d/1000);
		time_t_to_String(t, buf);
	}
	else
		return false;
	return rm.re->PartialMatch(p);
}
/* todo: internal dotted notation scans -- not done yet here. */
inline bool regexMatches(RegexMatcher& rm, Element& e, bool *deep) { 
	if( e.type() != Array ) 
		return _regexMatches(rm, e);

	JSElemIter ai(e.embeddedObject());
	while( ai.more() ) { 
		Element z = ai.next();
		if( _regexMatches(rm, z) ) {
			if( deep )
				*deep = true;
			return true;
		}
	}
	return false;
}

/* See if an object matches the query.
   deep - return true when meanswe looked into arrays for a match 
*/
bool JSMatcher::matches(JSObj& jsobj, bool *deep) {
	if( deep ) 
		*deep = false;

	/* assuming there is usually only one thing to match.  if more this
	could be slow sometimes. */

	// check normal non-regex cases:
	for( int i = 0; i < n; i++ ) {
		Element& m = toMatch[i]; 
		int cmp = matchesDotted(toMatch[i].fieldName(), toMatch[i], jsobj, compareOp[i], deep);

		/* missing is ok iff we were looking for null */
		if( cmp < 0 ) 
			return false;
		if( cmp == 0 && (m.type() != jstNULL && m.type() != Undefined ) )
			return false;
	}

	for( int r = 0; r < nRegex; r++ ) { 
		RegexMatcher& rm = regexs[r];
		Element e = jsobj.getFieldDotted(rm.fieldName);
		if( e.eoo() )
			return false;
		if( !regexMatches(rm, e, deep) )
			return false;
	}

	if( where ) { 
		if( where->func == 0 )
			return false; // didn't compile
		if( jsobj.objsize() < 200 || where->fullObject ) {
			JavaJS->scopeSetObject(where->scope, "obj", &jsobj);
		} else {
			JSObjBuilder b;
			where->buildSubset(jsobj, b);
			JSObj temp = b.done();
			JavaJS->scopeSetObject(where->scope, "obj", &temp);
		}
		if( JavaJS->invoke(where->scope, where->func) )
			return false;
		return JavaJS->scopeGetBoolean(where->scope, "return") != 0;
	}

	return true;
}

struct JSObj1 js1;

#pragma pack(push,1)
struct JSObj2 {
	JSObj2() {
		totsize=sizeof(JSObj2);
		s = String; strcpy_s(sname, 7, "abcdef"); slen = 10; 
		strcpy_s(sval, 10, "123456789"); eoo = EOO;
	}
	unsigned totsize;
	char s;
	char sname[7];
	unsigned slen;
	char sval[10];
	char eoo;
} js2;

struct JSUnitTest : public UnitTest {
	void run() {

		JSObj j1((const char *) &js1);
		JSObj j2((const char *) &js2);
		cout << "j1:" << j1.toString() << endl;
		cout << "j2:" << j2.toString() << endl;
		JSMatcher m(j2);
		assert( m.matches(j1) );
		js2.sval[0] = 'z';
		assert( !m.matches(j1) );
		JSMatcher n(j1);
		assert( n.matches(j1) );
		cout << "j2:" << j2.toString() << endl;
		assert( !n.matches(j2) );

cout << "temp1" << endl;
		JSObj j0 = emptyObj;
//		JSObj j0((const char *) &js0);
		JSMatcher p(j0);
		assert( p.matches(j1) );
		assert( p.matches(j2) );
	}
} jsunittest;

#pragma pack(pop)

struct RXTest : public UnitTest { 
	void run() { 
		/*
		static const boost::regex e("(\\d{4}[- ]){3}\\d{4}");
		static const boost::regex b(".....");
		cout << "regex result: " << regex_match("hello", e) << endl;
		cout << "regex result: " << regex_match("abcoo", b) << endl;
		*/
		pcrecpp::RE re1(")({a}h.*o");
		pcrecpp::RE re("h.llo");
		assert( re.FullMatch("hello") );
		assert( !re1.FullMatch("hello") );


		pcrecpp::RE_Options options;
		options.set_utf8(true);
		pcrecpp::RE part("dwi", options);
		assert( part.PartialMatch("dwight") );
	}
} rxtest;

