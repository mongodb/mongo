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
#include "../util/startup_test.h"
#include "diskloc.h"
#include "../scripting/engine.h"
#include "db.h"
#include "queryutil.h"
#include "client.h"

#include "pdfile.h"

namespace {
    inline pcrecpp::RE_Options flags2options(const char* flags) {
        pcrecpp::RE_Options options;
        options.set_utf8(true);
        while ( flags && *flags ) {
            if ( *flags == 'i' )
                options.set_caseless(true);
            else if ( *flags == 'm' )
                options.set_multiline(true);
            else if ( *flags == 'x' )
                options.set_extended(true);
            else if ( *flags == 's' )
                options.set_dotall(true);
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

            if ( scope.get() ){
                try {
                    scope->execSetup( "_mongo.readOnly = false;" , "make not read only" );
                }
                catch( DBException& e ){
                    warning() << "javascript scope cleanup interrupted" << causedBy( e ) << endl;
                }
            }

            if ( jsScope ) {
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
        delete _where;
        _where = 0;
    }

    ElementMatcher::ElementMatcher( BSONElement e , int op, bool isNot )
        : _toMatch( e ) , _compareOp( op ), _isNot( isNot ), _subMatcherOnPrimitives(false) {
        if ( op == BSONObj::opMOD ) {
            BSONObj o = e.embeddedObject();
            _mod = o["0"].numberInt();
            _modm = o["1"].numberInt();

            uassert( 10073 ,  "mod can't be 0" , _mod );
        }
        else if ( op == BSONObj::opTYPE ) {
            _type = (BSONType)(e.numberInt());
        }
        else if ( op == BSONObj::opELEM_MATCH ) {
            BSONElement m = e;
            uassert( 12517 , "$elemMatch needs an Object" , m.type() == Object );
            BSONObj x = m.embeddedObject();
            if ( x.firstElement().getGtLtOp() == 0 ) {
                _subMatcher.reset( new Matcher( x ) );
                _subMatcherOnPrimitives = false;
            }
            else {
                // meant to act on primitives
                _subMatcher.reset( new Matcher( BSON( "" << x ) ) );
                _subMatcherOnPrimitives = true;
            }
        }
    }

    ElementMatcher::ElementMatcher( BSONElement e , int op , const BSONObj& array, bool isNot )
        : _toMatch( e ) , _compareOp( op ), _isNot( isNot ), _subMatcherOnPrimitives(false) {

        _myset.reset( new set<BSONElement,element_lt>() );

        BSONObjIterator i( array );
        while ( i.more() ) {
            BSONElement ie = i.next();
            if ( op == BSONObj::opALL && ie.type() == Object && ie.embeddedObject().firstElement().getGtLtOp() == BSONObj::opELEM_MATCH ) {
                shared_ptr<Matcher> s;
                s.reset( new Matcher( ie.embeddedObject().firstElement().embeddedObjectUserCheck() ) );
                _allMatchers.push_back( s );
            }
            else if ( ie.type() == RegEx ) {
                if ( !_myregex.get() ) {
                    _myregex.reset( new vector< RegexMatcher >() );
                }
                _myregex->push_back( RegexMatcher() );
                RegexMatcher &rm = _myregex->back();
                rm._re.reset( new pcrecpp::RE( ie.regex(), flags2options( ie.regexFlags() ) ) );
                rm._fieldName = 0; // no need for field name
                rm._regex = ie.regex();
                rm._flags = ie.regexFlags();
                rm._isNot = false;
                bool purePrefix;
                string prefix = simpleRegex(rm._regex, rm._flags, &purePrefix);
                if (purePrefix)
                    rm._prefix = prefix;
            }
            else {
                uassert( 15882, "$elemMatch not allowed within $in",
                         ie.type() != Object ||
                         ie.embeddedObject().firstElement().getGtLtOp() != BSONObj::opELEM_MATCH );
                _myset->insert(ie);
            }
        }

        if ( _allMatchers.size() ) {
            uassert( 13020 , "with $all, can't mix $elemMatch and others" , _myset->size() == 0 && !_myregex.get());
        }

    }

    int ElementMatcher::inverseOfNegativeCompareOp() const {
        verify( negativeCompareOp() );
        return _compareOp == BSONObj::NE ? BSONObj::Equality : BSONObj::opIN;
    }

    bool ElementMatcher::negativeCompareOpContainsNull() const {
        verify( negativeCompareOp() );
        return (_compareOp == BSONObj::NE && _toMatch.type() != jstNULL) ||
        (_compareOp == BSONObj::NIN && _myset->count( staticNull.firstElement()) == 0 );
    }

    MatchDetails::MatchDetails() :
    _elemMatchKeyRequested() {
        resetOutput();
    }
    
    void MatchDetails::resetOutput() {
        _loadedRecord = false;
        _elemMatchKeyFound = false;
        _elemMatchKey = "";
    }
    
    string MatchDetails::toString() const {
        stringstream ss;
        ss << "loadedRecord: " << _loadedRecord << " ";
        ss << "elemMatchKeyRequested: " << _elemMatchKeyRequested << " ";
        ss << "elemMatchKey: " << ( _elemMatchKeyFound ? _elemMatchKey : "NONE" ) << " ";
        return ss.str();
    }
    
    void Matcher::addRegex(const char *fieldName, const char *regex, const char *flags, bool isNot) {

        RegexMatcher rm;
        rm._re.reset( new pcrecpp::RE(regex, flags2options(flags)) );
        rm._fieldName = fieldName;
        rm._regex = regex;
        rm._flags = flags;
        rm._isNot = isNot;
        _regexs.push_back(rm);

        if (!isNot) { //TODO something smarter
            bool purePrefix;
            string prefix = simpleRegex(regex, flags, &purePrefix);
            if (purePrefix)
                rm._prefix = prefix;
        }
    }

    bool Matcher::addOp( const BSONElement &e, const BSONElement &fe, bool isNot, const char *& regex, const char *&flags ) {
        const char *fn = fe.fieldName();
        int op = fe.getGtLtOp( -1 );
        if ( op == -1 ) {
            if ( !isNot && fn[1] == 'r' && fn[2] == 'e' && fn[3] == 'f' && fn[4] == 0 ) {
                return false; // { $ref : xxx } - treat as normal object
            }
            uassert( 10068 ,  (string)"invalid operator: " + fn , op != -1 );
        }

        switch ( op ) {
        case BSONObj::GT:
        case BSONObj::GTE:
        case BSONObj::LT:
        case BSONObj::LTE: {
            shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
            _builders.push_back( b );
            b->appendAs(fe, e.fieldName());
            addBasic(b->done().firstElement(), op, isNot);
            break;
        }
        case BSONObj::NE: {
            _haveNeg = true;
            shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
            _builders.push_back( b );
            b->appendAs(fe, e.fieldName());
            addBasic(b->done().firstElement(), BSONObj::NE, isNot);
            break;
        }
        case BSONObj::opALL:
            _all = true;
        case BSONObj::opIN: {
            uassert( 13276 , "$in needs an array" , fe.isABSONObj() );
            _basics.push_back( ElementMatcher( e , op , fe.embeddedObject(), isNot ) );
            BSONObjIterator i( fe.embeddedObject() );
            while( i.more() ) {
                if ( i.next().type() == Array ) {
                    _hasArray = true;
                }
            }
            break;
        }
        case BSONObj::NIN:
            uassert( 13277 , "$nin needs an array" , fe.isABSONObj() );
            _haveNeg = true;
            _basics.push_back( ElementMatcher( e , op , fe.embeddedObject(), isNot ) );
            break;
        case BSONObj::opMOD:
        case BSONObj::opTYPE:
        case BSONObj::opELEM_MATCH: {
            shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
            _builders.push_back( b );
            b->appendAs(fe, e.fieldName());
            // these are types where ElementMatcher has all the info
            _basics.push_back( ElementMatcher( b->done().firstElement() , op, isNot ) );
            break;
        }
        case BSONObj::opSIZE: {
            shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
            _builders.push_back( b );
            b->appendAs(fe, e.fieldName());
            addBasic(b->done().firstElement(), BSONObj::opSIZE, isNot);
            _haveSize = true;
            break;
        }
        case BSONObj::opEXISTS: {
            shared_ptr< BSONObjBuilder > b( new BSONObjBuilder() );
            _builders.push_back( b );
            b->appendAs(fe, e.fieldName());
            addBasic(b->done().firstElement(), BSONObj::opEXISTS, isNot);
            break;
        }
        case BSONObj::opREGEX: {
            uassert( 13032, "can't use $not with $regex, use BSON regex type instead", !isNot );
            if ( fe.type() == RegEx ) {
                regex = fe.regex();
                flags = fe.regexFlags();
            }
            else {
                regex = fe.valuestrsafe();
            }
            break;
        }
        case BSONObj::opOPTIONS: {
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

    void Matcher::parseExtractedClause( const BSONElement &e, list< shared_ptr< Matcher > > &matchers ) {
        uassert( 13086, "$and/$or/$nor must be a nonempty array", e.type() == Array && e.embeddedObject().nFields() > 0 );
        BSONObjIterator j( e.embeddedObject() );
        while( j.more() ) {
            BSONElement f = j.next();
            uassert( 13087, "$and/$or/$nor match element must be an object", f.type() == Object );
            matchers.push_back( shared_ptr< Matcher >( new Matcher( f.embeddedObject(), true ) ) );
        }
    }

    bool Matcher::parseClause( const BSONElement &e ) {
        const char *ef = e.fieldName();

        if ( ef[ 0 ] != '$' )
            return false;
        
        // $and
        if ( ef[ 1 ] == 'a' && ef[ 2 ] == 'n' && ef[ 3 ] == 'd' ) {
            parseExtractedClause( e, _andMatchers );
            return true;
        }

        // $or
        if ( ef[ 1 ] == 'o' && ef[ 2 ] == 'r' && ef[ 3 ] == 0 ) {
            parseExtractedClause( e, _orMatchers );
            return true;
        }
        
        // $nor
        if ( ef[ 1 ] == 'n' && ef[ 2 ] == 'o' && ef[ 3 ] == 'r' && ef[ 4 ] == 0 ) {
            parseExtractedClause( e, _norMatchers );
            return true;
        }
        
        // $comment
        if ( ef[ 1 ] == 'c' && ef[ 2 ] == 'o' && ef[ 3 ] == 'm' && str::equals( ef , "$comment" ) ) {
            return true;
        }

        return false;
    }

    // $where: function()...
    NOINLINE_DECL void Matcher::parseWhere( const BSONElement &e ) { 
        uassert(15902 , "$where expression has an unexpected type", e.type() == String || e.type() == CodeWScope || e.type() == Code );
        uassert( 10066 , "$where may only appear once in query", _where == 0 );
        uassert( 10067 , "$where query, but no script engine", globalScriptEngine );
        massert( 13089 , "no current client needed for $where" , haveClient() );
        _where = new Where();
        _where->scope = globalScriptEngine->getPooledScope( cc().ns() );
        _where->scope->localConnect( cc().database()->name.c_str() );
            
        if ( e.type() == CodeWScope ) {
            _where->setFunc( e.codeWScopeCode() );
            _where->jsScope = new BSONObj( e.codeWScopeScopeData() );
        }
        else {
            const char *code = e.valuestr();
            _where->setFunc(code);
        }
            
        _where->scope->execSetup( "_mongo.readOnly = true;" , "make read only" );
    }
    
    void Matcher::parseMatchExpressionElement( const BSONElement &e, bool nested ) {
        
        uassert( 13629 , "can't have undefined in a query expression" , e.type() != Undefined );
        
        if ( parseClause( e ) ) {
            return;   
        }

        const char *fn = e.fieldName();
        if ( str::equals(fn, "$where") ) {
            parseWhere(e);
            return;
        }

        if ( e.type() == RegEx ) {
            addRegex( fn, e.regex(), e.regexFlags() );
            return;
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
                        _haveNeg = true;
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
                    }
                    else {
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
            if (regex) {
                addRegex(e.fieldName(), regex, flags);
            }
            if ( isOperator )
                return;
        }
        
        if ( e.type() == Array ) {
            _hasArray = true;
        }
        else if( *fn == '$' ) {
            if( str::equals(fn, "$atomic") || str::equals(fn, "$isolated") ) {
                uassert( 14844, "$atomic specifier must be a top level field", !nested );
                _atomic = e.trueValue();
                return;
            }
        }
        
        // normal, simple case e.g. { a : "foo" }
        addBasic(e, BSONObj::Equality, false);
    }
    
    /* _jsobj          - the query pattern
    */
    Matcher::Matcher(const BSONObj &jsobj, bool nested) :
        _where(0), _jsobj(jsobj), _haveSize(), _all(), _hasArray(0), _haveNeg(), _atomic(false) {

        BSONObjIterator i(_jsobj);
        while ( i.more() ) {
            parseMatchExpressionElement( i.next(), nested );
        }
    }

    Matcher::Matcher( const Matcher &docMatcher, const BSONObj &key ) :
        _where(0), _constrainIndexKey( key ), _haveSize(), _all(), _hasArray(0), _haveNeg(), _atomic(false) {
        // Filter out match components that will provide an incorrect result
        // given a key from a single key index.
        for( vector< ElementMatcher >::const_iterator i = docMatcher._basics.begin(); i != docMatcher._basics.end(); ++i ) {
            if ( key.hasField( i->_toMatch.fieldName() ) ) {
                switch( i->_compareOp ) {
                case BSONObj::opSIZE:
                case BSONObj::opALL:
                case BSONObj::NE:
                case BSONObj::NIN:
                case BSONObj::opEXISTS: // We can't match on index in this case.
                case BSONObj::opTYPE: // For $type:10 (null), a null key could be a missing field or a null value field.
                    break;
                case BSONObj::opIN: {
                    bool inContainsArray = false;
                    for( set<BSONElement,element_lt>::const_iterator j = i->_myset->begin(); j != i->_myset->end(); ++j ) {
                        if ( j->type() == Array ) {
                            inContainsArray = true;
                            break;
                        }
                    }
                    // Can't match an array to its first indexed element.
                    if ( !i->_isNot && !inContainsArray ) {
                        _basics.push_back( *i );
                    }
                    break;
                }
                default: {
                    // Can't match an array to its first indexed element.
                    if ( !i->_isNot && i->_toMatch.type() != Array ) {
                        _basics.push_back( *i );
                    }
                }
                }
            }
        }
        for( vector<RegexMatcher>::const_iterator it = docMatcher._regexs.begin();
            it != docMatcher._regexs.end();
            ++it) {
            if ( !it->_isNot && key.hasField( it->_fieldName ) ) {
                _regexs.push_back(*it);
            }
        }
        // Recursively filter match components for and and or matchers.
        for( list< shared_ptr< Matcher > >::const_iterator i = docMatcher._andMatchers.begin(); i != docMatcher._andMatchers.end(); ++i ) {
            _andMatchers.push_back( shared_ptr< Matcher >( new Matcher( **i, key ) ) );
        }
        for( list< shared_ptr< Matcher > >::const_iterator i = docMatcher._orMatchers.begin(); i != docMatcher._orMatchers.end(); ++i ) {
            _orMatchers.push_back( shared_ptr< Matcher >( new Matcher( **i, key ) ) );
        }
    }

    inline bool regexMatches(const RegexMatcher& rm, const BSONElement& e) {
        switch (e.type()) {
        case String:
        case Symbol:
            if (rm._prefix.empty())
                return rm._re->PartialMatch(e.valuestr());
            else
                return !strncmp(e.valuestr(), rm._prefix.c_str(), rm._prefix.size());
        case RegEx:
            return !strcmp(rm._regex, e.regex()) && !strcmp(rm._flags, e.regexFlags());
        default:
            return false;
        }
    }

    inline int Matcher::valuesMatch(const BSONElement& l, const BSONElement& r, int op, const ElementMatcher& bm) const {
        verify( op != BSONObj::NE && op != BSONObj::NIN );

        if ( op == BSONObj::Equality ) {
            return l.valuesEqual(r);
        }

        if ( op == BSONObj::opIN ) {
            // { $in : [1,2,3] }
            int count = bm._myset->count(l);
            if ( count )
                return count;
            if ( bm._myregex.get() ) {
                for( vector<RegexMatcher>::const_iterator i = bm._myregex->begin(); i != bm._myregex->end(); ++i ) {
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

        if ( op == BSONObj::opMOD ) {
            if ( ! l.isNumber() )
                return false;

            return l.numberLong() % bm._mod == bm._modm;
        }

        if ( op == BSONObj::opTYPE ) {
            return bm._type == l.type();
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

    int Matcher::inverseMatch(const char *fieldName, const BSONElement &toMatch, const BSONObj &obj, const ElementMatcher& bm , MatchDetails * details ) const {
        int inverseRet = matchesDotted( fieldName, toMatch, obj, bm.inverseOfNegativeCompareOp(), bm , false , details );
        if ( bm.negativeCompareOpContainsNull() ) {
            return ( inverseRet <= 0 ) ? 1 : 0;
        }
        return -inverseRet;
    }

    int retExistsFound( const ElementMatcher &bm ) {
        return bm._toMatch.trueValue() ? 1 : -1;
    }

    /* Check if a particular field matches.

       fieldName - field to match "a.b" if we are reaching into an embedded object.
       toMatch   - element we want to match.
       obj       - database object to check against
       compareOp - Equality, LT, GT, etc.  This may be different than, and should supersede, the compare op in em. 
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
    int Matcher::matchesDotted(const char *fieldName, const BSONElement& toMatch, const BSONObj& obj, int compareOp, const ElementMatcher& em , bool isArr, MatchDetails * details ) const {
        DEBUGMATCHER( "\t matchesDotted : " << fieldName << " hasDetails: " << ( details ? "yes" : "no" ) );

        if ( compareOp == BSONObj::opALL ) {

            if ( em._allMatchers.size() ) {
                // $all query matching will not be performed against indexes, so the field
                // to match is always extracted from the full document.
                BSONElement e = obj.getFieldDotted( fieldName );
                // The $all/$elemMatch operator only matches arrays.
                if ( e.type() != Array ) {
                    return -1;
                }

                for ( unsigned i=0; i<em._allMatchers.size(); i++ ) {
                    bool found = false;
                    BSONObjIterator x( e.embeddedObject() );
                    while ( x.more() ) {
                        BSONElement f = x.next();

                        if ( f.type() != Object )
                            continue;
                        if ( em._allMatchers[i]->matches( f.embeddedObject() ) ) {
                            found = true;
                            break;
                        }
                    }

                    if ( ! found )
                        return -1;
                }

                return 1;
            }

            if ( em._myset->size() == 0 && !em._myregex.get() )
                return -1; // is this desired?

            BSONElementSet myValues;
            obj.getFieldsDotted( fieldName , myValues );

            for( set< BSONElement, element_lt >::const_iterator i = em._myset->begin(); i != em._myset->end(); ++i ) {
                // ignore nulls
                if ( i->type() == jstNULL )
                    continue;

                if ( myValues.count( *i ) == 0 )
                    return -1;
            }

            if ( !em._myregex.get() )
                return 1;

            for( vector< RegexMatcher >::const_iterator i = em._myregex->begin(); i != em._myregex->end(); ++i ) {
                bool match = false;
                for( BSONElementSet::const_iterator j = myValues.begin(); j != myValues.end(); ++j ) {
                    if ( regexMatches( *i, *j ) ) {
                        match = true;
                        break;
                    }
                }
                if ( !match )
                    return -1;
            }

            return 1;
        } // end opALL

        if ( compareOp == BSONObj::NE || compareOp == BSONObj::NIN ) {
            return inverseMatch( fieldName, toMatch, obj, em , details );
        }

        BSONElement e;
        bool indexed = !_constrainIndexKey.isEmpty();
        if ( indexed ) {
            e = obj.getFieldUsingIndexNames(fieldName, _constrainIndexKey);
            if( e.eoo() ) {
                cout << "obj: " << obj << endl;
                cout << "fieldName: " << fieldName << endl;
                cout << "_constrainIndexKey: " << _constrainIndexKey << endl;
                verify( !e.eoo() );
            }
        }
        else {

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

            // An array was encountered while scanning for components of the field name.
            if ( isArr ) {
                DEBUGMATCHER( "\t\t isArr 1 : obj : " << obj );
                BSONObjIterator ai(obj);
                bool found = false;
                while ( ai.moreWithEOO() ) {
                    BSONElement z = ai.next();

                    if( strcmp(z.fieldName(),fieldName) == 0 ) {
                        if ( compareOp == BSONObj::opEXISTS ) {
                                return retExistsFound( em );
                        }
                        if (valuesMatch(z, toMatch, compareOp, em) ) {
                                // "field.<n>" array notation was used
                            if ( details )
                                    details->setElemMatchKey( z.fieldName() );
                            return 1;
                        }
                    }

                    if ( z.type() == Object ) {
                        BSONObj eo = z.embeddedObject();
                        int cmp = matchesDotted(fieldName, toMatch, eo, compareOp, em, false, details );
                        if ( cmp > 0 ) {
                            if ( details )
                                details->setElemMatchKey( z.fieldName() );
                            return 1;
                        }
                        else if ( cmp < 0 ) {
                            found = true;
                        }
                    }
                }
                return found ? -1 : 0;
            }

            if( p ) {
                // Left portion of field name was not found or wrong type.
                return 0;
            }
            else {
                e = obj.getField(fieldName);
            }
        }

        if ( compareOp == BSONObj::opEXISTS ) {
            if( e.eoo() ) {
                return 0;
            } else {
                return retExistsFound( em );   
            }
        }
        else if ( ( e.type() != Array || indexed || compareOp == BSONObj::opSIZE ) &&
                  valuesMatch(e, toMatch, compareOp, em ) ) {
            return 1;
        }
        else if ( e.type() == Array && compareOp != BSONObj::opSIZE ) {
            BSONObjIterator ai(e.embeddedObject());

            while ( ai.moreWithEOO() ) {
                BSONElement z = ai.next();

                if ( compareOp == BSONObj::opELEM_MATCH ) {
                    if ( z.type() == Object ) {
                        if ( em._subMatcher->matches( z.embeddedObject() ) ) {
                            if ( details )
                                details->setElemMatchKey( z.fieldName() );
                            return 1;
                        }
                    }
                    else if ( em._subMatcherOnPrimitives ) {
                        if ( z.type() && em._subMatcher->matches( z.wrap( "" ) ) ) {
                            if ( details )
                                details->setElemMatchKey( z.fieldName() );
                            return 1;
                        }
                    }
                }
                else {
                    if ( valuesMatch( z, toMatch, compareOp, em) ) {
                        if ( details )
                            details->setElemMatchKey( z.fieldName() );
                        return 1;
                    }
                }

            }

            // match an entire array to itself
            if ( compareOp == BSONObj::Equality && e.woCompare( toMatch , false ) == 0 ) {
                return 1;
            }
            if ( compareOp == BSONObj::opIN && valuesMatch( e, toMatch, compareOp, em ) ) {
                return 1;
            }
        }
        else if ( e.eoo() ) {
            return 0;
        }
        return -1;
    }

    extern int dump;

    /* See if an object matches the query.
    */
    bool Matcher::matches(const BSONObj& jsobj , MatchDetails * details ) const {
        /*
          NB:  if any modifications are made to how this operates, make sure
          they are reflected in visitReferences(), whose implementation
          parallels this.
         */

        LOG(5) << "Matcher::matches() " << jsobj.toString() << endl;

        /* assuming there is usually only one thing to match.  if more this
           could be slow sometimes. */

        // check normal non-regex cases:
        for ( unsigned i = 0; i < _basics.size(); i++ ) {
            const ElementMatcher& bm = _basics[i];
            const BSONElement& m = bm._toMatch;
            // -1=mismatch. 0=missing element. 1=match
            int cmp = matchesDotted(m.fieldName(), m, jsobj, bm._compareOp, bm , false , details );
            if ( cmp == 0 && bm._compareOp == BSONObj::opEXISTS ) {
                // If missing, match cmp is opposite of $exists spec.
                cmp = -retExistsFound(bm);
            }
            if ( bm._isNot )
                cmp = -cmp;
            if ( cmp < 0 )
                return false;
            if ( cmp == 0 ) {
                /* missing is ok iff we were looking for null */
                if ( m.type() == jstNULL || m.type() == Undefined ||
                    ( ( bm._compareOp == BSONObj::opIN || bm._compareOp == BSONObj::NIN ) && bm._myset->count( staticNull.firstElement() ) > 0 ) ) {
                    if ( bm.negativeCompareOp() ^ bm._isNot ) {
                        return false;
                    }
                }
                else {
                    if ( !bm._isNot ) {
                        return false;
                    }
                }
            }
        }

        for (vector<RegexMatcher>::const_iterator it = _regexs.begin();
             it != _regexs.end();
             ++it) {
            BSONElementSet s;
            if ( !_constrainIndexKey.isEmpty() ) {
                BSONElement e = jsobj.getFieldUsingIndexNames(it->_fieldName, _constrainIndexKey);

                // Should only have keys nested one deep here, for geo-indices
                // TODO: future indices may nest deeper?
                if( e.type() == Array ){
                        BSONObjIterator i( e.Obj() );
                        while( i.more() ){
                                s.insert( i.next() );
                        }
                }
                else if ( !e.eoo() )
                    s.insert( e );

            }
            else {
                jsobj.getFieldsDotted( it->_fieldName, s );
            }
            bool match = false;
            for( BSONElementSet::const_iterator i = s.begin(); i != s.end(); ++i )
                if ( regexMatches(*it, *i) )
                    match = true;
            if ( !match ^ it->_isNot )
                return false;
        }

        if ( _orDedupConstraints.size() > 0 ) {
            for( vector< shared_ptr< FieldRangeVector > >::const_iterator i = _orDedupConstraints.begin();
                i != _orDedupConstraints.end(); ++i ) {
                if ( (*i)->matches( jsobj ) ) {
                    return false;
                }
            }
        }
        
        if ( _andMatchers.size() > 0 ) {
            for( list< shared_ptr< Matcher > >::const_iterator i = _andMatchers.begin();
                i != _andMatchers.end(); ++i ) {
                // SERVER-3192 Track field matched using details the same as for
                // top level fields, at least for now.
                if ( !(*i)->matches( jsobj, details ) ) {
                    return false;
                }
            }
        }

        if ( _orMatchers.size() > 0 ) {
            bool match = false;
            for( list< shared_ptr< Matcher > >::const_iterator i = _orMatchers.begin();
                    i != _orMatchers.end(); ++i ) {
                // SERVER-205 don't submit details - we don't want to track field
                // matched within $or
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
                // matched within $nor
                if ( (*i)->matches( jsobj ) ) {
                    return false;
                }
            }
        }

        if ( _where ) {
            if ( _where->func == 0 ) {
                uassert( 10070 , "$where compile error", false);
                return false; // didn't compile
            }

            if ( _where->jsScope ) {
                _where->scope->init( _where->jsScope );
            }
            _where->scope->setObject( "obj", const_cast< BSONObj & >( jsobj ) );
            _where->scope->setBoolean( "fullObject" , true ); // this is a hack b/c fullObject used to be relevant

            int err = _where->scope->invoke( _where->func , 0, &jsobj , 1000 * 60 , false );
            if ( err == -3 ) { // INVOKE_ERROR
                stringstream ss;
                ss << "error on invocation of $where function:\n"
                   << _where->scope->getError();
                uassert( 10071 , ss.str(), false);
                return false;
            }
            else if ( err != 0 ) {   // ! INVOKE_SUCCESS
                uassert( 10072 , "unknown error in invocation of $where function", false);
                return false;
            }
            return _where->scope->getBoolean( "return" ) != 0;

        }

        return true;
    }

#ifdef MONGO_LATER_SERVER_4644
    void Matcher::visitReferences(FieldSink *pSink) const {
        // check normal non-regex cases:
        for ( unsigned i = 0; i < _basics.size(); i++ ) {
            const ElementMatcher& bm = _basics[i];
            const BSONElement& m = bm._toMatch;
            // -1=mismatch. 0=missing element. 1=match
            int cmp = matchesDotted(m.fieldName(), m, jsobj, bm._compareOp, bm , false , details );
            if ( cmp == 0 && bm._compareOp == BSONObj::opEXISTS ) {
                // If missing, match cmp is opposite of $exists spec.
                cmp = -retExistsFound(bm);
            }
            if ( bm._isNot )
                cmp = -cmp;
            if ( cmp < 0 )
                return false;
            if ( cmp == 0 ) {
                /* missing is ok iff we were looking for null */
                if ( m.type() == jstNULL || m.type() == Undefined ||
                    ( ( bm._compareOp == BSONObj::opIN || bm._compareOp == BSONObj::NIN ) && bm._myset->count( staticNull.firstElement() ) > 0 ) ) {
                    if ( bm.negativeCompareOp() ^ bm._isNot ) {
                        return false;
                    }
                }
                else {
                    if ( !bm._isNot ) {
                        return false;
                    }
                }
            }
        }

        for (vector<RegexMatcher>::const_iterator it = _regexs.begin();
             it != _regexs.end();
             ++it) {
            BSONElementSet s;
            if ( !_constrainIndexKey.isEmpty() ) {
                BSONElement e = jsobj.getFieldUsingIndexNames(it->_fieldName, _constrainIndexKey);

                // Should only have keys nested one deep here, for geo-indices
                // TODO: future indices may nest deeper?
                if( e.type() == Array ){
                        BSONObjIterator i( e.Obj() );
                        while( i.more() ){
                                s.insert( i.next() );
                        }
                }
                else if ( !e.eoo() )
                    s.insert( e );

            }
            else {
                jsobj.getFieldsDotted( it->_fieldName, s );
            }
            bool match = false;
            for( BSONElementSet::const_iterator i = s.begin(); i != s.end(); ++i )
                if ( regexMatches(*it, *i) )
                    match = true;
            if ( !match ^ it->_isNot )
                return false;
        }

        if ( _orDedupConstraints.size() > 0 ) {
            for( vector< shared_ptr< FieldRangeVector > >::const_iterator i = _orDedupConstraints.begin();
                i != _orDedupConstraints.end(); ++i ) {
                if ( (*i)->matches( jsobj ) ) {
                    return false;
                }
            }
        }
        
        if ( _andMatchers.size() > 0 ) {
            for( list< shared_ptr< Matcher > >::const_iterator i = _andMatchers.begin();
                i != _andMatchers.end(); ++i ) {
                // SERVER-3192 Track field matched using details the same as for
                // top level fields, at least for now.
                if ( !(*i)->matches( jsobj, details ) ) {
                    return false;
                }
            }
        }

        if ( _orMatchers.size() > 0 ) {
            bool match = false;
            for( list< shared_ptr< Matcher > >::const_iterator i = _orMatchers.begin();
                    i != _orMatchers.end(); ++i ) {
                // SERVER-205 don't submit details - we don't want to track field
                // matched within $or
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
                // matched within $nor
                if ( (*i)->matches( jsobj ) ) {
                    return false;
                }
            }
        }

        if ( _where ) {
            if ( _where->func == 0 ) {
                u_assert( 10070 , "$where compile error", false);
                return false; // didn't compile
            }

            if ( _where->jsScope ) {
                _where->scope->init( _where->jsScope );
            }
            _where->scope->setObject( "obj", const_cast< BSONObj & >( jsobj ) );
            _where->scope->setBoolean( "fullObject" , true ); // this is a hack b/c fullObject used to be relevant

            int err = _where->scope->invoke( _where->func , 0, &jsobj , 1000 * 60 , false );
            if ( err == -3 ) { // INVOKE_ERROR
                stringstream ss;
                ss << "error on invocation of $where function:\n"
                   << _where->scope->getError();
                u_assert( 10071 , ss.str(), false);
                return false;
            }
            else if ( err != 0 ) {   // ! INVOKE_SUCCESS
                u_assert( 10072 , "unknown error in invocation of $where function", false);
                return false;
            }
            return _where->scope->getBoolean( "return" ) != 0;

        }
    }
#endif /* MONGO_LATER_SERVER_4644 */

    bool Matcher::keyMatch( const Matcher &docMatcher ) const {
        // Quick check certain non key match cases.
        if ( docMatcher._all
                || docMatcher._haveSize
                || docMatcher._hasArray // We can't match an array to its first indexed element using keymatch
                || docMatcher._haveNeg ) {
                return false;   
        }
        
        // Check that all match components are available in the index matcher.
        if ( !( _basics.size() == docMatcher._basics.size() && _regexs.size() == docMatcher._regexs.size() && !docMatcher._where ) ) {
            return false;
        }
        if ( _andMatchers.size() != docMatcher._andMatchers.size() ) {
            return false;
        }
        if ( _orMatchers.size() != docMatcher._orMatchers.size() ) {
            return false;
        }
        if ( docMatcher._norMatchers.size() > 0 ) {
            return false;
        }
        if ( docMatcher._orDedupConstraints.size() > 0 ) {
            return false;
        }
        
        // Recursively check that all submatchers support key match.
        {
            list< shared_ptr< Matcher > >::const_iterator i = _andMatchers.begin();
            list< shared_ptr< Matcher > >::const_iterator j = docMatcher._andMatchers.begin();
            while( i != _andMatchers.end() ) {
                if ( !(*i)->keyMatch( **j ) ) {
                    return false;
                }
                ++i; ++j;
            }
        }
        {
            list< shared_ptr< Matcher > >::const_iterator i = _orMatchers.begin();
            list< shared_ptr< Matcher > >::const_iterator j = docMatcher._orMatchers.begin();
            while( i != _orMatchers.end() ) {
                if ( !(*i)->keyMatch( **j ) ) {
                    return false;
                }
                ++i; ++j;
            }
        }
        // Nor matchers and or dedup constraints aren't created for index matchers,
        // so no need to check those here.
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
        little<unsigned> totsize;

        char n;
        char nname[5];
        little<double> N;

        char s;
        char sname[7];
        little<unsigned> slen;
        char sval[10];

        char eoo;
    };

    struct JSObj1 js1;
    
    struct JSObj2 {
        JSObj2() {
            totsize=sizeof(JSObj2);
            s = String;
            strcpy_s(sname, 7, "abcdef");
            slen = 10;
            strcpy_s(sval, 10, "123456789");
            eoo = EOO;
        }
        little<unsigned> totsize;
        char s;
        char sname[7];
        little<unsigned> slen;
        char sval[10];
        char eoo;
    } js2;
#pragma pack()

    struct JSUnitTest : public StartupTest {
        void run() {

            {
                // a quick check that we are using our mongo assert macro
                int x = 1;
                verify( ++x );
                if( x != 2 ) { 
                    log() << "bad build - wrong assert macro" << endl;
                    ::abort();    
                }
            }

            BSONObj j1((const char *) &js1);
            BSONObj j2((const char *) &js2);
            Matcher m(j2);
            verify( m.matches(j1) );
            js2.sval[0] = 'z';
            verify( !m.matches(j1) );
            Matcher n(j1);
            verify( n.matches(j1) );
            verify( !n.matches(j2) );

            BSONObj j0 = BSONObj();
//      BSONObj j0((const char *) &js0);
            Matcher p(j0);
            verify( p.matches(j1) );
            verify( p.matches(j2) );
        }
    } jsunittest;


    struct RXTest : public StartupTest {

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
            verify( re.FullMatch("hello") );
            verify( !re1.FullMatch("hello") );


            pcrecpp::RE_Options options;
            options.set_utf8(true);
            pcrecpp::RE part("dwi", options);
            verify( part.PartialMatch("dwight") );

            pcre_config( PCRE_CONFIG_UNICODE_PROPERTIES , &ret );
            if ( ! ret )
                cout << "warning: some regex utf8 things will not work.  pcre build doesn't have --enable-unicode-properties" << endl;

        }
    } rxtest;

} // namespace mongo
