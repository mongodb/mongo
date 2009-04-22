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
#include "storage.h"

namespace mongo {

#if defined(_WIN32)

} // namespace mongo

#include <hash_map>
using namespace stdext;

namespace mongo {

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

} // namespace mongo

#include <ext/hash_map>

namespace mongo {

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

//#include "minilex.h"
//MiniLex minilex;

    class Where {
    public:
        Where() {
            jsScope = 0;
        }
        ~Where() {
#if !defined(NOJNI)
            JavaJS->scopeFree(scope);
#endif
            if ( jsScope )
                delete jsScope;
            scope = 0;
            func = 0;
        }

        jlong scope, func;
        BSONObj *jsScope;

        void setFunc(const char *code) {
#if !defined(NOJNI)
            func = JavaJS->functionCreate( code );
#endif
        }

    };

    JSMatcher::~JSMatcher() {
        delete in;
        delete nin;
        delete all;
        delete where;
    }

} // namespace mongo

#include "pdfile.h"

namespace mongo {
    
    KeyValJSMatcher::KeyValJSMatcher(const BSONObj &_jsobj, const BSONObj &indexKeyPattern) :
    keyMatcher_(_jsobj.filterFieldsUndotted(indexKeyPattern, true), indexKeyPattern),
    recordMatcher_(_jsobj) {
    }
    
    bool KeyValJSMatcher::matches(const BSONObj &key, const DiskLoc &recLoc, bool *deep) {
        if ( keyMatcher_.keyMatch() ) {
            if ( !keyMatcher_.matches(key, deep) ) {
                return false;
            }
        }
        return recordMatcher_.matches(recLoc.rec(), deep);
    }
    
    
    /* _jsobj          - the query pattern
    */
    JSMatcher::JSMatcher(const BSONObj &_jsobj, const BSONObj &constrainIndexKey) :
            in(0), nin(0), all(0), where(0), jsobj(_jsobj), haveSize(), nRegex(0)
    {
        BSONObjIterator i(jsobj);
        n = 0;
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;

            if ( ( e.type() == CodeWScope || e.type() == Code ) && strcmp(e.fieldName(), "$where")==0 ) {
                // $where: function()...
                uassert( "$where occurs twice?", where == 0 );
                where = new Where();
                uassert( "$where query, but jni is disabled", JavaJS );
#if !defined(NOJNI)
                where->scope = JavaJS->scopeCreate();
                JavaJS->scopeSetString(where->scope, "$client", database->name.c_str());

                if ( e.type() == CodeWScope ) {
                    where->setFunc( e.codeWScopeCode() );
                    where->jsScope = new BSONObj( e.codeWScopeScopeData() , 0 );
                }
                else {
                    const char *code = e.valuestr();
                    where->setFunc(code);
                }
#endif
                continue;
            }

            if ( e.type() == RegEx ) {
                if ( nRegex >= 4 ) {
                    out() << "ERROR: too many regexes in query" << endl;
                }
                else {
                    pcrecpp::RE_Options options;
                    options.set_utf8(true);
                    const char *flags = e.regexFlags();
                    while ( flags && *flags ) {
                        if ( *flags == 'i' )
                            options.set_caseless(true);
                        else if ( *flags == 'm' )
                            options.set_multiline(true);
                        else if ( *flags == 'x' )
                            options.set_extended(true);
                        flags++;
                    }
                    RegexMatcher& rm = regexs[nRegex];
                    rm.re = new pcrecpp::RE(e.regex(), options);
                    rm.fieldName = e.fieldName();

                    nRegex++;
                }
                continue;
            }

            // greater than / less than...
            // e.g., e == { a : { $gt : 3 } }
            //       or
            //            { a : { $in : [1,2,3] } }
            if ( e.type() == Object ) {
                // e.g., fe == { $gt : 3 }
                BSONObjIterator j(e.embeddedObject());
                bool ok = false;
                while ( j.more() ) {
                    BSONElement fe = j.next();
                    if ( fe.eoo() )
                        break;
                    // BSONElement fe = e.embeddedObject().firstElement();
                    const char *fn = fe.fieldName();
                    /* TODO: use getGtLtOp() here.  this code repeats ourself */
                    if ( fn[0] == '$' && fn[1] ) {
                        if ( fn[2] == 't' ) {
                            int op = Equality;
                            if ( fn[1] == 'g' ) {
                                if ( fn[3] == 0 ) op = GT;
                                else if ( fn[3] == 'e' && fn[4] == 0 ) op = GTE;
                                else
                                    uassert("invalid $operator", false);
                            }
                            else if ( fn[1] == 'l' ) {
                                if ( fn[3] == 0 ) op = LT;
                                else if ( fn[3] == 'e' && fn[4] == 0 ) op = LTE;
                                else
                                    uassert("invalid $operator", false);
                            }
                            else
                                uassert("invalid $operator", false);
                            if ( op ) {
                                shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                                builders_.push_back( b );
                                b->appendAs(fe, e.fieldName());
                                addBasic(b->done().firstElement(), op);
                                ok = true;
                            }
                        }
                        else if ( fn[2] == 'e' ) {
                            if ( fn[1] == 'n' && fn[3] == 0 ) {
                                // $ne
                                shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                                builders_.push_back( b );
                                b->appendAs(fe, e.fieldName());
                                addBasic(b->done().firstElement(), NE);
                                ok = true;
                            }
                            else
                                uassert("invalid $operator", false);
                        }
                        else if ( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0 && fe.type() == Array ) {
                            // $in
                            uassert( "only 1 $in statement per query supported", in == 0 ); // todo...
                            in = new set<BSONElement,element_lt>();
                            BSONObjIterator i(fe.embeddedObject());
                            if ( i.more() ) {
                                while ( 1 ) {
                                    BSONElement ie = i.next();
                                    if ( ie.eoo() )
                                        break;
                                    in->insert(ie);
                                }
                            }
                            addBasic(e, opIN); // e not actually used at the moment for $in
                            ok = true;
                        }
                        else if ( fn[1] == 'n' && fn[2] == 'i' && fn[3] == 'n' && fn[4] == 0 && fe.type() == Array ) {
                            // $nin
                            uassert( "only 1 $nin statement per query supported", nin == 0 ); // todo...
                            nin = new set<BSONElement,element_lt>();
                            BSONObjIterator i(fe.embeddedObject());
                            if ( i.more() ) {
                                while ( 1 ) {
                                    BSONElement ie = i.next();
                                    if ( ie.eoo() )
                                        break;
                                    nin->insert(ie);
                                }
                            }
                            addBasic(e, NIN); // e not actually used at the moment for $nin
                            ok = true;
                        }
                        else if ( fn[1] == 'a' && fn[2] == 'l' && fn[3] == 'l' && fn[4] == 0 && fe.type() == Array ) {
                            // $all
                            uassert( "only 1 $all statement per query supported", all == 0 ); // todo...
                            all = new set<BSONElement,element_lt>();
                            BSONObjIterator i(fe.embeddedObject());
                            if ( i.more() ) {
                                while ( 1 ) {
                                    BSONElement ie = i.next();
                                    if ( ie.eoo() )
                                        break;
                                    all->insert(ie);
                                }
                            }
                            addBasic(e, opALL); // e not actually used at the moment for $all
                            ok = true;
                        }
                        else if ( fn[1] == 's' && fn[2] == 'i' && fn[3] == 'z' && fn[4] == 'e' && fe.isNumber() ) {
                            shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                            builders_.push_back( b );
                            b->appendAs(fe, e.fieldName());
                            addBasic(b->done().firstElement(), opSIZE);    
                            haveSize = true;
                            ok = true;
                        }
                        else
                            uassert("invalid $operator", false);
                    }
                    else {
                        ok = false;
                        break;
                    }
                }
                if ( ok )
                    continue;
            }

            // normal, simple case e.g. { a : "foo" }
            addBasic(e, Equality);
        }
        
        if ( keyMatch() )
            constrainIndexKey_ = constrainIndexKey;
    }

    inline int JSMatcher::valuesMatch(const BSONElement& l, const BSONElement& r, int op, bool *deep) {
        assert( op != NE && op != NIN );
        
        if ( op == 0 )
            return l.valuesEqual(r);

        if ( op == opIN ) {
            // { $in : [1,2,3] }
            int c = in->count(l);
            return c;
        }

        if ( op == opSIZE ) {
            if ( l.type() != Array )
                return 0;
            int count = 0;
            BSONObjIterator i( l.embeddedObject() );
            while( i.more() ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                ++count;
            }
            return count == r.number();
        }
        
        if ( op == opALL ) {
            if ( l.type() != Array )
                return 0;
            set< BSONElement, element_lt > matches;
            BSONObjIterator i( l.embeddedObject() );
            while( i.more() ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                if ( all->count( e ) )
                    matches.insert( e );
            }
            if ( all->size() == matches.size() ) {
                if ( deep )
                    *deep = true;
                return true;
            }
            return false;
        }
        
        /* check LT, GTE, ... */
        if ( !( l.isNumber() && r.isNumber() ) && ( l.type() != r.type() ) )
            return false;
        int c = compareElementValues(l, r);
        if ( c < -1 ) c = -1;
        if ( c > 1 ) c = 1;
        int z = 1 << (c+1);
        return (op & z);
    }

    int JSMatcher::matchesNe(const char *fieldName, const BSONElement &toMatch, const BSONObj &obj, bool *deep) {
        int ret = matchesDotted( fieldName, toMatch, obj, Equality, deep );
        return -ret;
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
    int JSMatcher::matchesDotted(const char *fieldName, const BSONElement& toMatch, const BSONObj& obj, int compareOp, bool *deep, bool isArr) {
        if ( compareOp == NE )
            return matchesNe( fieldName, toMatch, obj, deep );
        if ( compareOp == NIN ) {
            for( set<BSONElement,element_lt>::const_iterator i = nin->begin(); i != nin->end(); ++i ) {
                int ret = matchesNe( fieldName, *i, obj, deep );
                if ( ret != 1 )
                    return ret;
                // code to handle 0 (missing) return value doesn't deal with nin yet
            }
            return 1;
        }
        
        BSONElement e;
        if ( !constrainIndexKey_.isEmpty() ) {
            e = obj.getFieldUsingIndexNames(fieldName, constrainIndexKey_);
            assert( !e.eoo() );
        } else {
            if ( isArr ) {
                BSONObjIterator ai(obj);
                bool found = false;
                while ( ai.more() ) {
                    BSONElement z = ai.next();
                    if ( z.type() == Object ) {
                        BSONObj eo = z.embeddedObject();
                        int cmp = matchesDotted(fieldName, toMatch, eo, compareOp, deep);
                        if ( cmp > 0 ) {
                            if ( deep ) *deep = true;
                            return 1;
                        } else if ( cmp < 0 ) {
                            found = true;
                        }
                    }
                }
                return found ? -1 : 0;
            }
            const char *p = strchr(fieldName, '.');
            if ( p ) {
                string left(fieldName, p-fieldName);

                BSONElement se = obj.getField(left.c_str());
                if ( se.eoo() )
                    return 0;
                if ( se.type() != Object && se.type() != Array )
                    return -1;

                BSONObj eo = se.embeddedObject();
                return matchesDotted(p+1, toMatch, eo, compareOp, deep, se.type() == Array);
            } else {
                e = obj.getField(fieldName);
            }
        }
        
        if ( valuesMatch(e, toMatch, compareOp, deep) ) {
            return 1;
        } else if ( e.type() == Array && compareOp != opALL && compareOp != opSIZE ) {
            BSONObjIterator ai(e.embeddedObject());
            while ( ai.more() ) {
                BSONElement z = ai.next();
                if ( valuesMatch( z, toMatch, compareOp) ) {
                    if ( deep )
                        *deep = true;
                    return 1;
                }
            }
        }
        else if ( e.eoo() ) {
            // 0 indicates "missing element"
            return 0;
        }
        return -1;
    }

    extern int dump;

    inline bool _regexMatches(RegexMatcher& rm, const BSONElement& e) {
        char buf[64];
        const char *p = buf;
        if ( e.type() == String || e.type() == Symbol )
            p = e.valuestr();
        else if ( e.isNumber() ) {
            sprintf(buf, "%f", e.number());
        }
        else if ( e.type() == Date ) {
            unsigned long long d = e.date();
            time_t t = (d/1000);
            time_t_to_String(t, buf);
        }
        else
            return false;
        return rm.re->PartialMatch(p);
    }
    /* todo: internal dotted notation scans -- not done yet here. */
    inline bool regexMatches(RegexMatcher& rm, const BSONElement& e, bool *deep) {
        if ( e.type() != Array )
            return _regexMatches(rm, e);

        BSONObjIterator ai(e.embeddedObject());
        while ( ai.more() ) {
            BSONElement z = ai.next();
            if ( _regexMatches(rm, z) ) {
                if ( deep )
                    *deep = true;
                return true;
            }
        }
        return false;
    }

    /* See if an object matches the query.
       deep - return true when means we looked into arrays for a match
    */
    bool JSMatcher::matches(const BSONObj& jsobj, bool *deep) {
        if ( deep )
            *deep = false;

        /* assuming there is usually only one thing to match.  if more this
        could be slow sometimes. */

        // check normal non-regex cases:
        for ( int i = 0; i < n; i++ ) {
            BasicMatcher& bm = basics[i];
            BSONElement& m = bm.toMatch;
            // -1=mismatch. 0=missing element. 1=match
            int cmp = matchesDotted(m.fieldName(), m, jsobj, bm.compareOp, deep);
            if ( cmp < 0 )
                return false;
            if ( cmp == 0 ) {
                /* missing is ok iff we were looking for null */
                if ( m.type() == jstNULL || m.type() == Undefined ) {
                    if ( bm.compareOp == NE ) {
                        return false;
                    }
                } else {
                    return false;
                }
            }
        }

        for ( int r = 0; r < nRegex; r++ ) {
            RegexMatcher& rm = regexs[r];
            BSONElement e;
            if ( !constrainIndexKey_.isEmpty() )
                e = jsobj.getFieldUsingIndexNames(rm.fieldName, constrainIndexKey_);
            else
                e = jsobj.getFieldDotted(rm.fieldName);
            if ( e.eoo() )
                return false;
            if ( !regexMatches(rm, e, deep) )
                return false;
        }

        if ( where ) {
            if ( where->func == 0 ) {
                uassert("$where compile error", false);
                return false; // didn't compile
            }
#if !defined(NOJNI)

            /**if( 1 || jsobj.objsize() < 200 || where->fullObject ) */
            {
                if ( where->jsScope ) {
                    JavaJS->scopeInit( where->scope , where->jsScope );
                }
                JavaJS->scopeSetThis( where->scope, const_cast< BSONObj * >( &jsobj ) );
                JavaJS->scopeSetObject( where->scope, "obj", const_cast< BSONObj * >( &jsobj ) );
            }
            /*else {
            BSONObjBuilder b;
            where->buildSubset(jsobj, b);
            BSONObj temp = b.done();
            JavaJS->scopeSetObject(where->scope, "obj", &temp);
            }*/
            int err = JavaJS->invoke(where->scope, where->func);
            if ( err == -3 ) { // INVOKE_ERROR
                stringstream ss;
                ss << "error on invocation of $where function:\n" 
                    << JavaJS->scopeGetString(where->scope, "error");
                uassert(ss.str(), false);
                return false;
            } else if ( err != 0 ) { // ! INVOKE_SUCCESS
                uassert("error in invocation of $where function", false);
                return false;                
            }
            return JavaJS->scopeGetBoolean(where->scope, "return") != 0;
#else
            return false;
#endif
        }

        return true;
    }

    struct JSObj1 js1;

#pragma pack(1)
    struct JSObj2 {
        JSObj2() {
            totsize=sizeof(JSObj2);
            s = String;
            strcpy_s(sname, 7, "abcdef");
            slen = 10;
            strcpy_s(sval, 10, "123456789");
            eoo = EOO;
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

            BSONObj j1((const char *) &js1);
            BSONObj j2((const char *) &js2);
            JSMatcher m(j2);
            assert( m.matches(j1) );
            js2.sval[0] = 'z';
            assert( !m.matches(j1) );
            JSMatcher n(j1);
            assert( n.matches(j1) );
            assert( !n.matches(j2) );

            BSONObj j0 = BSONObj();
//		BSONObj j0((const char *) &js0);
            JSMatcher p(j0);
            assert( p.matches(j1) );
            assert( p.matches(j2) );
        }
    } jsunittest;

#pragma pack()

    struct RXTest : public UnitTest {

        RXTest() {
        }

        void run() {
            /*
            static const boost::regex e("(\\d{4}[- ]){3}\\d{4}");
            static const boost::regex b(".....");
            out() << "regex result: " << regex_match("hello", e) << endl;
            out() << "regex result: " << regex_match("abcoo", b) << endl;
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

} // namespace mongo
