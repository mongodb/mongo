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
#include "matcher.h"
#include "../util/goodies.h"
#include "../util/unittest.h"
#include "storage.h"

#include "../scripting/engine.h"

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

            if ( jsScope ){
                delete jsScope;
            }
            func = 0;
        }
        
        auto_ptr<Scope> scope;
        ScriptingFunction func;
        BSONObj *jsScope;
        
        void setFunc(const char *code) {
            massert( "scope has to be created first!" , scope.get() );
            func = scope->createFunction( code );
        }

    };

    JSMatcher::~JSMatcher() {
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
        where(0), jsobj(_jsobj), haveSize(), all(), nRegex(0){

        BSONObjIterator i(jsobj);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;

            if ( ( e.type() == CodeWScope || e.type() == Code || e.type() == String ) && strcmp(e.fieldName(), "$where")==0 ) {
                // $where: function()...
                uassert( "$where occurs twice?", where == 0 );
                where = new Where();
                uassert( "$where query, but no script engine", globalScriptEngine );

                assert( curNs );
                where->scope = globalScriptEngine->getPooledScope( curNs );
                where->scope->localConnect( database->name.c_str() );

                if ( e.type() == CodeWScope ) {
                    where->setFunc( e.codeWScopeCode() );
                    where->jsScope = new BSONObj( e.codeWScopeScopeData() , 0 );
                }
                else {
                    const char *code = e.valuestr();
                    where->setFunc(code);
                }

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
                while ( j.moreWithEOO() ) {
                    BSONElement fe = j.next();
                    if ( fe.eoo() )
                        break;
                    // BSONElement fe = e.embeddedObject().firstElement();
                    const char *fn = fe.fieldName();
                    /* TODO: use getGtLtOp() here.  this code repeats ourself */
                    if ( fn[0] == '$' && fn[1] ) {
                        if ( fn[2] == 't' ) {
                            int op = BSONObj::Equality;
                            if ( fn[1] == 'g' ) {
                                if ( fn[3] == 0 ) op = BSONObj::GT;
                                else if ( fn[3] == 'e' && fn[4] == 0 ) op = BSONObj::GTE;
                                else
                                    uassert("invalid $operator", false);
                            }
                            else if ( fn[1] == 'l' ) {
                                if ( fn[3] == 0 ) op = BSONObj::LT;
                                else if ( fn[3] == 'e' && fn[4] == 0 ) op = BSONObj::LTE;
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
                                addBasic(b->done().firstElement(), BSONObj::NE);
                                ok = true;
                            }
                            else
                                uassert("invalid $operator", false);
                        }
                        else if ( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0 && fe.type() == Array ) {
                            // $in
                            basics.push_back( BasicMatcher( e , BSONObj::opIN , fe.embeddedObject() ) );
                            ok = true;
                        }
                        else if ( fn[1] == 'n' && fn[2] == 'i' && fn[3] == 'n' && fn[4] == 0 && fe.type() == Array ) {
                            // $nin
                            basics.push_back( BasicMatcher( e , BSONObj::NIN , fe.embeddedObject() ) );
                            ok = true;
                        }
                        else if ( fn[1] == 'a' && fn[2] == 'l' && fn[3] == 'l' && fn[4] == 0 && fe.type() == Array ) {
                            // $all
                            basics.push_back( BasicMatcher( e , BSONObj::opALL , fe.embeddedObject() ) );
                            ok = true;
                            all = true;
                        }
                        else if ( fn[1] == 's' && fn[2] == 'i' && fn[3] == 'z' && fn[4] == 'e' && fe.isNumber() ) {
                            shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                            builders_.push_back( b );
                            b->appendAs(fe, e.fieldName());
                            addBasic(b->done().firstElement(), BSONObj::opSIZE);    
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
            addBasic(e, BSONObj::Equality);
        }
        
        constrainIndexKey_ = constrainIndexKey;
    }

    inline int JSMatcher::valuesMatch(const BSONElement& l, const BSONElement& r, int op, const BasicMatcher& bm, bool *deep) {
        assert( op != BSONObj::NE && op != BSONObj::NIN );
        
        if ( op == 0 )
            return l.valuesEqual(r);
        
        if ( op == BSONObj::opIN ) {
            // { $in : [1,2,3] }
            return bm.myset->count(l);
        }

        if ( op == BSONObj::opSIZE ) {
            if ( l.type() != Array )
                return 0;
            int count = 0;
            BSONObjIterator i( l.embeddedObject() );
            while( i.moreWithEOO() ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                ++count;
            }
            return count == r.number();
        }
        
        if ( op == BSONObj::opALL ) {
            if ( l.type() != Array )
                return 0;
            set< BSONElement, element_lt > matches;
            BSONObjIterator i( l.embeddedObject() );
            while( i.moreWithEOO() ) {
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                if ( bm.myset->count( e ) )
                    matches.insert( e );
            }
            if ( matches.size() > 0 && bm.myset->size() == matches.size() ) {
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

    int JSMatcher::matchesNe(const char *fieldName, const BSONElement &toMatch, const BSONObj &obj, const BasicMatcher& bm, bool *deep) {
        int ret = matchesDotted( fieldName, toMatch, obj, BSONObj::Equality, bm, deep );
        if ( ret <= 0 )
            return 1;
        return 0;
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
    int JSMatcher::matchesDotted(const char *fieldName, const BSONElement& toMatch, const BSONObj& obj, int compareOp, const BasicMatcher& bm , bool *deep, bool isArr) {

        if ( compareOp == BSONObj::NE )
            return matchesNe( fieldName, toMatch, obj, bm, deep );
        if ( compareOp == BSONObj::NIN ) {
            for( set<BSONElement,element_lt>::const_iterator i = bm.myset->begin(); i != bm.myset->end(); ++i ) {
                int ret = matchesNe( fieldName, *i, obj, bm, deep );
                if ( ret != 1 )
                    return ret;
                // code to handle 0 (missing) return value doesn't deal with nin yet
            }
            return 1;
        }
        
        BSONElement e;
        bool indexed = !constrainIndexKey_.isEmpty();
        if ( indexed ) {
            e = obj.getFieldUsingIndexNames(fieldName, constrainIndexKey_);
            assert( !e.eoo() );
        } else {
            if ( isArr ) {
                BSONObjIterator ai(obj);
                bool found = false;
                while ( ai.moreWithEOO() ) {
                    BSONElement z = ai.next();
                    if ( z.type() == Object ) {
                        BSONObj eo = z.embeddedObject();
                        int cmp = matchesDotted(fieldName, toMatch, eo, compareOp, bm, deep, false);
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
                return matchesDotted(p+1, toMatch, eo, compareOp, bm, deep, se.type() == Array);
            } else {
                e = obj.getField(fieldName);
            }
        }
        
        if ( ( e.type() != Array || indexed || compareOp == BSONObj::opALL || compareOp == BSONObj::opSIZE ) &&
            valuesMatch(e, toMatch, compareOp, bm, deep) ) {
            return 1;
        } else if ( e.type() == Array && compareOp != BSONObj::opALL && compareOp != BSONObj::opSIZE ) {
            BSONObjIterator ai(e.embeddedObject());
            while ( ai.moreWithEOO() ) {
                BSONElement z = ai.next();
                if ( valuesMatch( z, toMatch, compareOp, bm) ) {
                    if ( deep )
                        *deep = true;
                    return 1;
                }
            }
            if ( compareOp == BSONObj::Equality && e.woCompare( toMatch ) == 0 )
                return 1;
        }
        else if ( e.eoo() ) {
            // 0 indicates "missing element"
            return 0;
        }
        return -1;
    }

    extern int dump;

    inline bool regexMatches(RegexMatcher& rm, const BSONElement& e) {
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

    /* See if an object matches the query.
       deep - return true when means we looked into arrays for a match
    */
    bool JSMatcher::matches(const BSONObj& jsobj, bool *deep) {
        if ( deep )
            *deep = false;

        /* assuming there is usually only one thing to match.  if more this
        could be slow sometimes. */

        // check normal non-regex cases:
        for ( unsigned i = 0; i < basics.size(); i++ ) {
            BasicMatcher& bm = basics[i];
            BSONElement& m = bm.toMatch;
            // -1=mismatch. 0=missing element. 1=match
            int cmp = matchesDotted(m.fieldName(), m, jsobj, bm.compareOp, bm, deep);
            if ( cmp < 0 )
                return false;
            if ( cmp == 0 ) {
                /* missing is ok iff we were looking for null */
                if ( m.type() == jstNULL || m.type() == Undefined ) {
                    if ( bm.compareOp == BSONObj::NE ) {
                        return false;
                    }
                } else {
                    return false;
                }
            }
        }

        for ( int r = 0; r < nRegex; r++ ) {
            RegexMatcher& rm = regexs[r];
            BSONElementSet s;
            if ( !constrainIndexKey_.isEmpty() ) {
                BSONElement e = jsobj.getFieldUsingIndexNames(rm.fieldName, constrainIndexKey_);
                if ( !e.eoo() )
                    s.insert( e );
            } else {
                jsobj.getFieldsDotted( rm.fieldName, s, deep );
            }
            bool match = false;
            for( BSONElementSet::const_iterator i = s.begin(); i != s.end(); ++i )
                if ( regexMatches(rm, *i) )
                    match = true;
            if ( !match )
                return false;
        }
        
        if ( where ) {
            if ( where->func == 0 ) {
                uassert("$where compile error", false);
                return false; // didn't compile
            }

            /**if( 1 || jsobj.objsize() < 200 || where->fullObject ) */
            {
                if ( where->jsScope ){
                    where->scope->init( where->jsScope );
                }
                where->scope->setThis( const_cast< BSONObj * >( &jsobj ) );
                where->scope->setObject( "obj", const_cast< BSONObj & >( jsobj ) );
                where->scope->setBoolean( "fullObject" , true ); // this is a hack b/c fullObject used to be relevant
            }
            /*else {
            BSONObjBuilder b;
            where->buildSubset(jsobj, b);
            BSONObj temp = b.done();
            where->scope->setObject( "obj" , &temp );
            }*/
            int err = where->scope->invoke( where->func , BSONObj() , 1000 * 60 );
            if ( err == -3 ) { // INVOKE_ERROR
                stringstream ss;
                ss << "error on invocation of $where function:\n" 
                   << where->scope->getError();
                uassert(ss.str(), false);
                return false;
            } else if ( err != 0 ) { // ! INVOKE_SUCCESS
                uassert("unknown error in invocation of $where function", false);
                return false;                
            }
            return where->scope->getBoolean( "return" ) != 0;

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

            int ret = 0;
            
            pcre_config( PCRE_CONFIG_UTF8 , &ret );
            massert( "pcre not compiled with utf8 support" , ret );

            pcrecpp::RE re1(")({a}h.*o");
            pcrecpp::RE re("h.llo");
            assert( re.FullMatch("hello") );
            assert( !re1.FullMatch("hello") );


            pcrecpp::RE_Options options;
            options.set_utf8(true);
            pcrecpp::RE part("dwi", options);
            assert( part.PartialMatch("dwight") );

            pcre_config( PCRE_CONFIG_UNICODE_PROPERTIES , &ret );
            if ( ! ret )
                cerr << "warning: some regex utf8 things will not work.  pcre build doesn't have --enable-unicode-properties" << endl;
            
        }
    } rxtest;

} // namespace mongo
