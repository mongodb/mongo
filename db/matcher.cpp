// matcher.cpp

/* Matcher is our boolean expression evaluator for "where" clauses */

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

#include "pch.h"
#include "matcher.h"
#include "../util/goodies.h"
#include "../util/unittest.h"
#include "diskloc.h"
#include "../scripting/engine.h"
#include "db.h"
#include "client.h"

#include "pdfile.h"

namespace {
    inline pcrecpp::RE_Options flags2options(const char* flags){
        pcrecpp::RE_Options options;
        options.set_utf8(true);
        while ( flags && *flags ) {
            if ( *flags == 'i' )
                options.set_caseless(true);
            else if ( *flags == 'm' )
                options.set_multiline(true);
            else if ( *flags == 'x' )
                options.set_extended(true);
            flags++;
        }
        return options;
    }
}

//#define DEBUGMATCHER(x) cout << x << endl;
#define DEBUGMATCHER(x)

namespace mongo {

    extern BSONObj staticNull;
        
    class Where {
    public:
        Where() {
            jsScope = 0;
            func = 0;
        }
        ~Where() {

            if ( scope.get() )
                scope->execSetup( "_mongo.readOnly = false;" , "make not read only" );

            if ( jsScope ){
                delete jsScope;
                jsScope = 0;
            }
            func = 0;
        }
        
        auto_ptr<Scope> scope;
        ScriptingFunction func;
        BSONObj *jsScope;
        
        void setFunc(const char *code) {
            massert( 10341 ,  "scope has to be created first!" , scope.get() );
            func = scope->createFunction( code );
        }
        
    };

    Matcher::~Matcher() {
        delete where;
        where = 0;
    }

    ElementMatcher::ElementMatcher( BSONElement _e , int _op, bool _isNot ) : toMatch( _e ) , compareOp( _op ), isNot( _isNot ) {
        if ( _op == BSONObj::opMOD ){
            BSONObj o = _e.embeddedObject();
            mod = o["0"].numberInt();
            modm = o["1"].numberInt();
            
            uassert( 10073 ,  "mod can't be 0" , mod );
        }
        else if ( _op == BSONObj::opTYPE ){
            type = (BSONType)(_e.numberInt());
        }
        else if ( _op == BSONObj::opELEM_MATCH ){
            BSONElement m = _e;
            uassert( 12517 , "$elemMatch needs an Object" , m.type() == Object );
            subMatcher.reset( new Matcher( m.embeddedObject() ) );
        }
    }

    ElementMatcher::ElementMatcher( BSONElement _e , int _op , const BSONObj& array, bool _isNot ) 
        : toMatch( _e ) , compareOp( _op ), isNot( _isNot ) {
        
        myset.reset( new set<BSONElement,element_lt>() );
        
        BSONObjIterator i( array );
        while ( i.more() ) {
            BSONElement ie = i.next();
            if ( _op == BSONObj::opALL && ie.type() == Object && ie.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ){
                shared_ptr<Matcher> s;
                s.reset( new Matcher( ie.embeddedObject().firstElement().embeddedObjectUserCheck() ) );
                allMatchers.push_back( s );
            } else if ( ie.type() == RegEx ) {
                if ( !myregex.get() ) {
                    myregex.reset( new vector< RegexMatcher >() );
                }
                myregex->push_back( RegexMatcher() );
                RegexMatcher &rm = myregex->back();
                rm.re.reset( new pcrecpp::RE( ie.regex(), flags2options( ie.regexFlags() ) ) );
                rm.fieldName = 0; // no need for field name
                rm.regex = ie.regex();
                rm.flags = ie.regexFlags();
                rm.isNot = false;
                bool purePrefix;
                string prefix = simpleRegex(rm.regex, rm.flags, &purePrefix);
                if (purePrefix)
                    rm.prefix = prefix;
            } else {
                myset->insert(ie);
            }
        }
        
        if ( allMatchers.size() ){
            uassert( 13020 , "with $all, can't mix $elemMatch and others" , myset->size() == 0 && !myregex.get());
        }
        
    }
    
    
    void Matcher::addRegex(const char *fieldName, const char *regex, const char *flags, bool isNot){

        if ( nRegex >= 4 ) {
            out() << "ERROR: too many regexes in query" << endl;
        }
        else {
            RegexMatcher& rm = regexs[nRegex];
            rm.re.reset( new pcrecpp::RE(regex, flags2options(flags)) );
            rm.fieldName = fieldName;
            rm.regex = regex;
            rm.flags = flags;
            rm.isNot = isNot;
            nRegex++;

            if (!isNot){ //TODO something smarter
                bool purePrefix;
                string prefix = simpleRegex(regex, flags, &purePrefix);
                if (purePrefix)
                    rm.prefix = prefix;
            }
        }        
    }
    
    bool Matcher::addOp( const BSONElement &e, const BSONElement &fe, bool isNot, const char *& regex, const char *&flags ) {
        const char *fn = fe.fieldName();
        int op = fe.getGtLtOp( -1 );
        if ( op == -1 ){
            if ( !isNot && fn[1] == 'r' && fn[2] == 'e' && fn[3] == 'f' && fn[4] == 0 ){
                return false; // { $ref : xxx } - treat as normal object
            }
            uassert( 10068 ,  (string)"invalid operator: " + fn , op != -1 );
        }
        
        switch ( op ){
            case BSONObj::GT:
            case BSONObj::GTE:
            case BSONObj::LT:
            case BSONObj::LTE:{
                shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                _builders.push_back( b );
                b->appendAs(fe, e.fieldName());
                addBasic(b->done().firstElement(), op, isNot);
                break;
            }
            case BSONObj::NE:{
                haveNeg = true;
                shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                _builders.push_back( b );
                b->appendAs(fe, e.fieldName());
                addBasic(b->done().firstElement(), BSONObj::NE, isNot);
                break;
            }
            case BSONObj::opALL:
                all = true;
            case BSONObj::opIN:
                uassert( 13276 , "$in needs an array" , fe.isABSONObj() );
                basics.push_back( ElementMatcher( e , op , fe.embeddedObject(), isNot ) );
                break;
            case BSONObj::NIN:
                uassert( 13277 , "$nin needs an array" , fe.isABSONObj() );
                haveNeg = true;
                basics.push_back( ElementMatcher( e , op , fe.embeddedObject(), isNot ) );
                break;
            case BSONObj::opMOD:
            case BSONObj::opTYPE:
            case BSONObj::opELEM_MATCH: {
                shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                _builders.push_back( b );
                b->appendAs(fe, e.fieldName());                                
                // these are types where ElementMatcher has all the info
                basics.push_back( ElementMatcher( b->done().firstElement() , op, isNot ) );
                break;                                
            }
            case BSONObj::opSIZE:{
                shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                _builders.push_back( b );
                b->appendAs(fe, e.fieldName());
                addBasic(b->done().firstElement(), BSONObj::opSIZE, isNot);    
                haveSize = true;
                break;
            }
            case BSONObj::opEXISTS:{
                shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
                _builders.push_back( b );
                b->appendAs(fe, e.fieldName());
                addBasic(b->done().firstElement(), BSONObj::opEXISTS, isNot);
                break;
            }
            case BSONObj::opREGEX:{
                uassert( 13032, "can't use $not with $regex, use BSON regex type instead", !isNot );
                if ( fe.type() == RegEx ){
                    regex = fe.regex();
                    flags = fe.regexFlags();
                }
                else {
                    regex = fe.valuestrsafe();
                }
                break;
            }
            case BSONObj::opOPTIONS:{
                uassert( 13029, "can't use $not with $options, use BSON regex type instead", !isNot );
                flags = fe.valuestrsafe();
                break;
            }
            case BSONObj::opNEAR:
            case BSONObj::opWITHIN:
            case BSONObj::opMAX_DISTANCE:
                break;
            default:
                uassert( 10069 ,  (string)"BUG - can't operator for: " + fn , 0 );
        }        
        return true;
    }
    
    void Matcher::parseOr( const BSONElement &e, bool subMatcher, list< shared_ptr< Matcher > > &matchers ) {
        uassert( 13090, "nested $or/$nor not allowed", !subMatcher );
        uassert( 13086, "$or/$nor must be a nonempty array", e.type() == Array && e.embeddedObject().nFields() > 0 );
        BSONObjIterator j( e.embeddedObject() );
        while( j.more() ) {
            BSONElement f = j.next();
            uassert( 13087, "$or/$nor match element must be an object", f.type() == Object );
            // until SERVER-109 this is never a covered index match, so don't constrain index key for $or matchers
            matchers.push_back( shared_ptr< Matcher >( new Matcher( f.embeddedObject(), true ) ) );
        }
    }

    bool Matcher::parseOrNor( const BSONElement &e, bool subMatcher ) {
        const char *ef = e.fieldName();
        if ( ef[ 0 ] != '$' )
            return false;
        if ( ef[ 1 ] == 'o' && ef[ 2 ] == 'r' && ef[ 3 ] == 0 ) {
            parseOr( e, subMatcher, _orMatchers );
        } else if ( ef[ 1 ] == 'n' && ef[ 2 ] == 'o' && ef[ 3 ] == 'r' && ef[ 4 ] == 0 ) {
            parseOr( e, subMatcher, _norMatchers );
        } else {
            return false;
        }
        return true;
    }
    
    /* _jsobj          - the query pattern
    */
    Matcher::Matcher(const BSONObj &_jsobj, bool subMatcher) :
        where(0), jsobj(_jsobj), haveSize(), all(), hasArray(0), haveNeg(), _atomic(false), nRegex(0) {

        BSONObjIterator i(jsobj);
        while ( i.more() ) {
            BSONElement e = i.next();
            
            if ( parseOrNor( e, subMatcher ) ) {
                continue;
            }

            if ( ( e.type() == CodeWScope || e.type() == Code || e.type() == String ) && strcmp(e.fieldName(), "$where")==0 ) {
                // $where: function()...
                uassert( 10066 , "$where occurs twice?", where == 0 );
                uassert( 10067 , "$where query, but no script engine", globalScriptEngine );
                massert( 13089 , "no current client needed for $where" , haveClient() ); 
                where = new Where();
                where->scope = globalScriptEngine->getPooledScope( cc().ns() );
                where->scope->localConnect( cc().database()->name.c_str() );

                if ( e.type() == CodeWScope ) {
                    where->setFunc( e.codeWScopeCode() );
                    where->jsScope = new BSONObj( e.codeWScopeScopeData() , 0 );
                }
                else {
                    const char *code = e.valuestr();
                    where->setFunc(code);
                }
                
                where->scope->execSetup( "_mongo.readOnly = true;" , "make read only" );

                continue;
            }

            if ( e.type() == RegEx ) {
                addRegex( e.fieldName(), e.regex(), e.regexFlags() );
                continue;
            }
            
            // greater than / less than...
            // e.g., e == { a : { $gt : 3 } }
            //       or
            //            { a : { $in : [1,2,3] } }
            if ( e.type() == Object ) {
                // support {$regex:"a|b", $options:"imx"}
                const char* regex = NULL;
                const char* flags = "";
                
                // e.g., fe == { $gt : 3 }
                BSONObjIterator j(e.embeddedObject());
                bool isOperator = false;
                while ( j.more() ) {
                    BSONElement fe = j.next();
                    const char *fn = fe.fieldName();
                    
                    if ( fn[0] == '$' && fn[1] ) {
                        isOperator = true;
                        
                        if ( fn[1] == 'n' && fn[2] == 'o' && fn[3] == 't' && fn[4] == 0 ) {
                            haveNeg = true;
                            switch( fe.type() ) {
                                case Object: {
                                    BSONObjIterator k( fe.embeddedObject() );
                                    uassert( 13030, "$not cannot be empty", k.more() );
                                    while( k.more() ) {
                                        addOp( e, k.next(), true, regex, flags );   
                                    }
                                    break;
                                }
                                case RegEx:
                                    addRegex( e.fieldName(), fe.regex(), fe.regexFlags(), true );
                                    break;
                                default:
                                    uassert( 13031, "invalid use of $not", false );
                            }
                        } else {
                            if ( !addOp( e, fe, false, regex, flags ) ) {
                                isOperator = false;
                                break;
                            }
                        }
                    }
                    else {
                        isOperator = false;
                        break;
                    }
                }
                if (regex){
                    addRegex(e.fieldName(), regex, flags);
                }
                if ( isOperator )
                    continue;
            }

            if ( e.type() == Array ){
                hasArray = true;
            }
            else if( strcmp(e.fieldName(), "$atomic") == 0 ) {
                _atomic = e.trueValue();
                continue;
            }
            
            // normal, simple case e.g. { a : "foo" }
            addBasic(e, BSONObj::Equality, false);
        }
    }
    
    Matcher::Matcher( const Matcher &other, const BSONObj &key ) :
    where(0), constrainIndexKey_( key ), haveSize(), all(), hasArray(0), haveNeg(), _atomic(false), nRegex(0) {
        // do not include fields which would make keyMatch() false
        for( vector< ElementMatcher >::const_iterator i = other.basics.begin(); i != other.basics.end(); ++i ) {
            if ( key.hasField( i->toMatch.fieldName() ) ) {
                switch( i->compareOp ) {
                    case BSONObj::opSIZE:
                    case BSONObj::opALL:
                    case BSONObj::NE:
                    case BSONObj::NIN:
                        break;
                    default: {
                        if ( !i->isNot && i->toMatch.type() != Array ) {
                            basics.push_back( *i );                            
                        }
                    }
                }
            }
        }
        for( int i = 0; i < other.nRegex; ++i ) {
            if ( !other.regexs[ i ].isNot && key.hasField( other.regexs[ i ].fieldName ) ) {
                regexs[ nRegex++ ] = other.regexs[ i ];
            }
        }
        for( list< shared_ptr< Matcher > >::const_iterator i = other._orMatchers.begin(); i != other._orMatchers.end(); ++i ) {
            _orMatchers.push_back( shared_ptr< Matcher >( new Matcher( **i, key ) ) );
        }
    }
    
    inline bool regexMatches(const RegexMatcher& rm, const BSONElement& e) {
        switch (e.type()){
            case String:
            case Symbol:
                if (rm.prefix.empty())
                    return rm.re->PartialMatch(e.valuestr());
                else
                    return !strncmp(e.valuestr(), rm.prefix.c_str(), rm.prefix.size());
            case RegEx:
                return !strcmp(rm.regex, e.regex()) && !strcmp(rm.flags, e.regexFlags());
            default:
                return false;
        }
    }
        
    inline int Matcher::valuesMatch(const BSONElement& l, const BSONElement& r, int op, const ElementMatcher& bm) {
        assert( op != BSONObj::NE && op != BSONObj::NIN );
        
        if ( op == BSONObj::Equality ) {
            return l.valuesEqual(r);
        }
        
        if ( op == BSONObj::opIN ) {
            // { $in : [1,2,3] }
            int count = bm.myset->count(l);
            if ( count )
                return count;
            if ( bm.myregex.get() ) {
                for( vector<RegexMatcher>::const_iterator i = bm.myregex->begin(); i != bm.myregex->end(); ++i ) {
                    if ( regexMatches( *i, l ) ) {
                        return true;
                    }
                }
            }
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
        
        if ( op == BSONObj::opMOD ){
            if ( ! l.isNumber() )
                return false;
            
            return l.numberLong() % bm.mod == bm.modm;
        }
        
        if ( op == BSONObj::opTYPE ){
            return bm.type == l.type();
        }

        /* check LT, GTE, ... */
        if ( l.canonicalType() != r.canonicalType() )
            return false;
        int c = compareElementValues(l, r);
        if ( c < -1 ) c = -1;
        if ( c > 1 ) c = 1;
        int z = 1 << (c+1);
        return (op & z);
    }

    int Matcher::matchesNe(const char *fieldName, const BSONElement &toMatch, const BSONObj &obj, const ElementMatcher& bm , MatchDetails * details ) {
        int ret = matchesDotted( fieldName, toMatch, obj, BSONObj::Equality, bm , false , details );
        if ( bm.toMatch.type() != jstNULL )
            return ( ret <= 0 ) ? 1 : 0;
        else
            return -ret;
    }

    int retMissing( const ElementMatcher &bm ) {
        if ( bm.compareOp != BSONObj::opEXISTS )
            return 0;
        return bm.toMatch.boolean() ? -1 : 1;
    }
    
    /* Check if a particular field matches.

       fieldName - field to match "a.b" if we are reaching into an embedded object.
       toMatch   - element we want to match.
       obj       - database object to check against
       compareOp - Equality, LT, GT, etc.
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
    int Matcher::matchesDotted(const char *fieldName, const BSONElement& toMatch, const BSONObj& obj, int compareOp, const ElementMatcher& em , bool isArr, MatchDetails * details ) {
        DEBUGMATCHER( "\t matchesDotted : " << fieldName << " hasDetails: " << ( details ? "yes" : "no" ) );
        if ( compareOp == BSONObj::opALL ) {
            
            if ( em.allMatchers.size() ){
                BSONElement e = obj.getFieldDotted( fieldName );
                uassert( 13021 , "$all/$elemMatch needs to be applied to array" , e.type() == Array );
                
                for ( unsigned i=0; i<em.allMatchers.size(); i++ ){
                    bool found = false;
                    BSONObjIterator x( e.embeddedObject() );
                    while ( x.more() ){
                        BSONElement f = x.next();

                        if ( f.type() != Object )
                            continue;
                        if ( em.allMatchers[i]->matches( f.embeddedObject() ) ){
                            found = true;
                            break;
                        }
                    }

                    if ( ! found )
                        return -1;
                }
                
                return 1;
            }
            
            if ( em.myset->size() == 0 && !em.myregex.get() )
                return -1; // is this desired?
            
            BSONObjSetDefaultOrder actualKeys;
            IndexSpec( BSON( fieldName << 1 ) ).getKeys( obj, actualKeys );
            if ( actualKeys.size() == 0 )
                return 0;
            
            for( set< BSONElement, element_lt >::const_iterator i = em.myset->begin(); i != em.myset->end(); ++i ) {
                // ignore nulls
                if ( i->type() == jstNULL )
                    continue;
                // parallel traversal would be faster worst case I guess
                BSONObjBuilder b;
                b.appendAs( *i, "" );
                if ( !actualKeys.count( b.done() ) )
                    return -1;
            }

            if ( !em.myregex.get() )
                return 1;
            
            for( vector< RegexMatcher >::const_iterator i = em.myregex->begin(); i != em.myregex->end(); ++i ) {
                bool match = false;
                for( BSONObjSetDefaultOrder::const_iterator j = actualKeys.begin(); j != actualKeys.end(); ++j ) {
                    if ( regexMatches( *i, j->firstElement() ) ) {
                        match = true;
                        break;
                    }
                }
                if ( !match )
                    return -1;
            }
            
            return 1;
        } // end opALL
        
        if ( compareOp == BSONObj::NE )
            return matchesNe( fieldName, toMatch, obj, em , details );
        if ( compareOp == BSONObj::NIN ) {
            for( set<BSONElement,element_lt>::const_iterator i = em.myset->begin(); i != em.myset->end(); ++i ) {
                int ret = matchesNe( fieldName, *i, obj, em , details );
                if ( ret != 1 )
                    return ret;
            }
            if ( em.myregex.get() ) {
                BSONElementSet s;
                obj.getFieldsDotted( fieldName, s );
                for( vector<RegexMatcher>::const_iterator i = em.myregex->begin(); i != em.myregex->end(); ++i ) {
                    for( BSONElementSet::const_iterator j = s.begin(); j != s.end(); ++j ) {
                        if ( regexMatches( *i, *j ) ) {
                            return -1;
                        }
                    }
                }
            }
            return 1;
        }
        
        BSONElement e;
        bool indexed = !constrainIndexKey_.isEmpty();
        if ( indexed ) {
            e = obj.getFieldUsingIndexNames(fieldName, constrainIndexKey_);
            if( e.eoo() ){
                cout << "obj: " << obj << endl;
                cout << "fieldName: " << fieldName << endl;
                cout << "constrainIndexKey_: " << constrainIndexKey_ << endl;
                assert( !e.eoo() );
            }
        } else {

            const char *p = strchr(fieldName, '.');
            if ( p ) {
                string left(fieldName, p-fieldName);

                BSONElement se = obj.getField(left.c_str());
                if ( se.eoo() )
                    ;
                else if ( se.type() != Object && se.type() != Array )
                    ;
                else {
                    BSONObj eo = se.embeddedObject();
                    return matchesDotted(p+1, toMatch, eo, compareOp, em, se.type() == Array , details );
                }
            }

            if ( isArr ) {
                DEBUGMATCHER( "\t\t isArr 1 : obj : " << obj );
                BSONObjIterator ai(obj);
                bool found = false;
                while ( ai.moreWithEOO() ) {
                    BSONElement z = ai.next();

                    if( strcmp(z.fieldName(),fieldName) == 0 && valuesMatch(z, toMatch, compareOp, em) ) {
                        // "field.<n>" array notation was used
                        if ( details )
                            details->elemMatchKey = z.fieldName();
                        return 1;
                    }

                    if ( z.type() == Object ) {
                        BSONObj eo = z.embeddedObject();
                        int cmp = matchesDotted(fieldName, toMatch, eo, compareOp, em, false, details );
                        if ( cmp > 0 ) {
                            if ( details )
                                details->elemMatchKey = z.fieldName();
                            return 1;
                        } 
                        else if ( cmp < 0 ) {
                            found = true;
                        }
                    }
                }
                return found ? -1 : retMissing( em );
            }

            if( p ) { 
                return retMissing( em );
            }
            else {
                e = obj.getField(fieldName);
            }
        }

        if ( compareOp == BSONObj::opEXISTS ) {
            return ( e.eoo() ^ ( toMatch.boolean() ^ em.isNot ) ) ? 1 : -1;
        } else if ( ( e.type() != Array || indexed || compareOp == BSONObj::opSIZE ) &&
            valuesMatch(e, toMatch, compareOp, em ) ) {
            return 1;
        } else if ( e.type() == Array && compareOp != BSONObj::opSIZE ) {
            BSONObjIterator ai(e.embeddedObject());

            while ( ai.moreWithEOO() ) {
                BSONElement z = ai.next();
                
                if ( compareOp == BSONObj::opELEM_MATCH ){
                    // SERVER-377
                    if ( z.type() == Object && em.subMatcher->matches( z.embeddedObject() ) ){
                        if ( details )
                            details->elemMatchKey = z.fieldName();
                        return 1;
                    }
                }
                else {
                    if ( valuesMatch( z, toMatch, compareOp, em) ) {
                        if ( details )
                            details->elemMatchKey = z.fieldName();
                        return 1;
                    }
                }

            }
            
            if ( compareOp == BSONObj::Equality && e.woCompare( toMatch , false ) == 0 ){
                // match an entire array to itself
                return 1;
            }
            
        }
        else if ( e.eoo() ) {
            // 0 indicates "missing element"
            return 0;
        }
        return -1;
    }

    extern int dump;

    /* See if an object matches the query.
    */
    bool Matcher::matches(const BSONObj& jsobj , MatchDetails * details ) {
        /* assuming there is usually only one thing to match.  if more this
        could be slow sometimes. */

        // check normal non-regex cases:
        for ( unsigned i = 0; i < basics.size(); i++ ) {
            ElementMatcher& bm = basics[i];
            BSONElement& m = bm.toMatch;
            // -1=mismatch. 0=missing element. 1=match
            int cmp = matchesDotted(m.fieldName(), m, jsobj, bm.compareOp, bm , false , details );
            if ( bm.compareOp != BSONObj::opEXISTS && bm.isNot )
                cmp = -cmp;
            if ( cmp < 0 )
                return false;
            if ( cmp == 0 ) {
                /* missing is ok iff we were looking for null */
                if ( m.type() == jstNULL || m.type() == Undefined || ( bm.compareOp == BSONObj::opIN && bm.myset->count( staticNull.firstElement() ) > 0 ) ) {
                    if ( ( bm.compareOp == BSONObj::NE ) ^ bm.isNot ) {
                        return false;
                    }
                } else {
                    if ( !bm.isNot ) {
                        return false;
                    }
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
                jsobj.getFieldsDotted( rm.fieldName, s );
            }
            bool match = false;
            for( BSONElementSet::const_iterator i = s.begin(); i != s.end(); ++i )
                if ( regexMatches(rm, *i) )
                    match = true;
            if ( !match ^ rm.isNot )
                return false;
        }
        
        if ( _orMatchers.size() > 0 ) {
            bool match = false;
            for( list< shared_ptr< Matcher > >::const_iterator i = _orMatchers.begin();
                i != _orMatchers.end(); ++i ) {
                // SERVER-205 don't submit details - we don't want to track field
                // matched within $or, and at this point we've already loaded the
                // whole document
                if ( (*i)->matches( jsobj ) ) {
                    match = true;
                    break;
                }
            }
            if ( !match ) {
                return false;
            }
        }
        
        if ( _norMatchers.size() > 0 ) {
            for( list< shared_ptr< Matcher > >::const_iterator i = _norMatchers.begin();
                i != _norMatchers.end(); ++i ) {
                // SERVER-205 don't submit details - we don't want to track field
                // matched within $nor, and at this point we've already loaded the
                // whole document
                if ( (*i)->matches( jsobj ) ) {
                    return false;
                }
            }            
        }
        
        for( vector< shared_ptr< FieldRangeVector > >::const_iterator i = _orConstraints.begin();
            i != _orConstraints.end(); ++i ) {
            if ( (*i)->matches( jsobj ) ) {
                return false;
            }
        }
                
        if ( where ) {
            if ( where->func == 0 ) {
                uassert( 10070 , "$where compile error", false);
                return false; // didn't compile
            }
            
            if ( where->jsScope ){
                where->scope->init( where->jsScope );
            }
            where->scope->setThis( const_cast< BSONObj * >( &jsobj ) );
            where->scope->setObject( "obj", const_cast< BSONObj & >( jsobj ) );
            where->scope->setBoolean( "fullObject" , true ); // this is a hack b/c fullObject used to be relevant
            
            int err = where->scope->invoke( where->func , BSONObj() , 1000 * 60 , false );
            where->scope->setThis( 0 );
            if ( err == -3 ) { // INVOKE_ERROR
                stringstream ss;
                ss << "error on invocation of $where function:\n" 
                   << where->scope->getError();
                uassert( 10071 , ss.str(), false);
                return false;
            } else if ( err != 0 ) { // ! INVOKE_SUCCESS
                uassert( 10072 , "unknown error in invocation of $where function", false);
                return false;                
            }
            return where->scope->getBoolean( "return" ) != 0;

        }
        
        return true;
    }

    bool Matcher::hasType( BSONObj::MatchType type ) const {
        for ( unsigned i=0; i<basics.size() ; i++ )
            if ( basics[i].compareOp == type )
                return true;
        return false;
    }

    bool Matcher::sameCriteriaCount( const Matcher &other ) const {
        if ( !( basics.size() == other.basics.size() && nRegex == other.nRegex && !where == !other.where ) ) {
            return false;
        }
        if ( _norMatchers.size() != other._norMatchers.size() ) {
            return false;
        }
        if ( _orMatchers.size() != other._orMatchers.size() ) {
            return false;
        }
        if ( _orConstraints.size() != other._orConstraints.size() ) {
            return false;
        }
        {
            list< shared_ptr< Matcher > >::const_iterator i = _norMatchers.begin();
            list< shared_ptr< Matcher > >::const_iterator j = other._norMatchers.begin();
            while( i != _norMatchers.end() ) {
                if ( !(*i)->sameCriteriaCount( **j ) ) {
                    return false;
                }
                ++i; ++j;
            }
        }
        {
            list< shared_ptr< Matcher > >::const_iterator i = _orMatchers.begin();
            list< shared_ptr< Matcher > >::const_iterator j = other._orMatchers.begin();
            while( i != _orMatchers.end() ) {
                if ( !(*i)->sameCriteriaCount( **j ) ) {
                    return false;
                }
                ++i; ++j;
            }
        }
        return true;
    }        
        
    
    /*- just for testing -- */
#pragma pack(1)
    struct JSObj1 {
        JSObj1() {
            totsize=sizeof(JSObj1);
            n = NumberDouble;
            strcpy_s(nname, 5, "abcd");
            N = 3.1;
            s = String;
            strcpy_s(sname, 7, "abcdef");
            slen = 10;
            strcpy_s(sval, 10, "123456789");
            eoo = EOO;
        }
        unsigned totsize;

        char n;
        char nname[5];
        double N;

        char s;
        char sname[7];
        unsigned slen;
        char sval[10];

        char eoo;
    };
#pragma pack()

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
            Matcher m(j2);
            assert( m.matches(j1) );
            js2.sval[0] = 'z';
            assert( !m.matches(j1) );
            Matcher n(j1);
            assert( n.matches(j1) );
            assert( !n.matches(j2) );

            BSONObj j0 = BSONObj();
//		BSONObj j0((const char *) &js0);
            Matcher p(j0);
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
            massert( 10342 ,  "pcre not compiled with utf8 support" , ret );

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
                cout << "warning: some regex utf8 things will not work.  pcre build doesn't have --enable-unicode-properties" << endl;
            
        }
    } rxtest;

} // namespace mongo
