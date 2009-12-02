/** @file jsobj.cpp - BSON implementation
    http://www.mongodb.org/display/DOCS/BSON
*/

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "stdafx.h"
#include "jsobj.h"
#include "nonce.h"
#include "../util/goodies.h"
#include "../util/base64.h"
#include "../util/md5.hpp"
#include <limits>
#include "../util/unittest.h"
#include "../util/embedded_builder.h"
#include "json.h"
#include "jsobjmanipulator.h"
#include "../util/optime.h"
#include <boost/static_assert.hpp>
#undef assert
#define assert xassert

// make sure our assumptions are valid
BOOST_STATIC_ASSERT( sizeof(int) == 4 );
BOOST_STATIC_ASSERT( sizeof(long long) == 8 );
BOOST_STATIC_ASSERT( sizeof(double) == 8 );
BOOST_STATIC_ASSERT( sizeof(mongo::OID) == 12 );

namespace mongo {

    BSONElement nullElement;

    ostream& operator<<( ostream &s, const OID &o ) {
        s << o.str();
        return s;
    }

    IDLabeler GENOID;
    BSONObjBuilder& operator<<(BSONObjBuilder& b, IDLabeler& id) {
        OID oid;
        oid.init();
        b.appendOID("_id", &oid);
        return b;
    }

    DateNowLabeler DATENOW;

    string BSONElement::toString( bool includeFieldName ) const {
        stringstream s;
        if ( includeFieldName && type() != EOO )
            s << fieldName() << ": ";
        switch ( type() ) {
        case EOO:
            return "EOO";
        case Date:
            s << "new Date(" << date() << ')';
            break;
        case RegEx:
            {
                s << "/" << regex() << '/';
                const char *p = regexFlags();
                if ( p ) s << p;
            }
            break;
        case NumberDouble:
            {
                stringstream tmp;
                tmp.precision( 16 );
                tmp << number();
                string n = tmp.str();
                s << n;
                // indicate this is a double:
                if( strchr(n.c_str(), '.') == 0 && strchr(n.c_str(), 'E') == 0 && strchr(n.c_str(), 'N') == 0 )
                    s << ".0";
            }
            break;
        case NumberLong:
            s << _numberLong();
            break;
        case NumberInt:
            s << _numberInt();
            break;
        case Bool:
            s << ( boolean() ? "true" : "false" );
            break;
        case Object:
        case Array:
            s << embeddedObject().toString();
            break;
        case Undefined:
            s << "undefined";
            break;
        case jstNULL:
            s << "null";
            break;
        case MaxKey:
            s << "MaxKey";
            break;
        case MinKey:
            s << "MinKey";
            break;
        case CodeWScope:
            s << "CodeWScope( "
                << codeWScopeCode() << ", " << codeWScopeObject().toString() << ")";
            break;
        case Code:
            if ( valuestrsize() > 80 )
                s << string(valuestr()).substr(0, 70) << "...";
            else {
                s << valuestr();
            }
            break;
        case Symbol:
        case String:
            if ( valuestrsize() > 80 )
                s << '"' << string(valuestr()).substr(0, 70) << "...\"";
            else {
                s << '"' << valuestr() << '"';
            }
            break;
        case DBRef:
            s << "DBRef('" << valuestr() << "',";
            {
                OID *x = (OID *) (valuestr() + valuestrsize());
                s << *x << ')';
            }
            break;
        case jstOID:
            s << "ObjId(";
            s << __oid() << ')';
            break;
        case BinData:
            s << "BinData";
            break;
        case Timestamp:
            s << "Timestamp " << timestampTime() << "|" << timestampInc();
            break;
        default:
            s << "?type=" << type();
            break;
        }
        return s.str();
    }

    string escape( string s ) {
        stringstream ret;
        for ( string::iterator i = s.begin(); i != s.end(); ++i ) {
            switch ( *i ) {
            case '"':
                ret << "\\\"";
                break;
            case '\\':
                ret << "\\\\";
                break;
            case '/':
                ret << "\\/";
                break;
            case '\b':
                ret << "\\b";
                break;
            case '\f':
                ret << "\\f";
                break;
            case '\n':
                ret << "\\n";
                break;
            case '\r':
                ret << "\\r";
                break;
            case '\t':
                ret << "\\t";
                break;
            default:
                if ( *i >= 0 && *i <= 0x1f ) {
                    ret << "\\u";
                    ret << hex;
                    ret.width( 4 );
                    ret.fill( '0' );
                    ret << int( *i );
                } else {
                    ret << *i;
                }
            }
        }
        return ret.str();
    }

    string BSONElement::jsonString( JsonStringFormat format, bool includeFieldNames ) const {
        stringstream s;
        if ( includeFieldNames )
            s << '"' << escape( fieldName() ) << "\" : ";
        switch ( type() ) {
        case String:
        case Symbol:
            s << '"' << escape( valuestr() ) << '"';
            break;
        case NumberLong:
            s << _numberLong();
            break;
        case NumberInt:
        case NumberDouble:
            if ( number() >= -numeric_limits< double >::max() &&
                    number() <= numeric_limits< double >::max() ) {
                s.precision( 16 );
                s << number();
            } else {
                stringstream ss;
                ss << "Number " << number() << " cannot be represented in JSON";
                string message = ss.str();
                massert( message.c_str(), false );
            }
            break;
        case Bool:
            s << ( boolean() ? "true" : "false" );
            break;
        case jstNULL:
            s << "null";
            break;
        case Object:
            s << embeddedObject().jsonString( format );
            break;
        case Array: {
            if ( embeddedObject().isEmpty() ) {
                s << "[]";
                break;
            }
            s << "[ ";
            BSONObjIterator i( embeddedObject() );
            BSONElement e = i.next();
            if ( !e.eoo() )
                while ( 1 ) {
                    s << e.jsonString( format, false );
                    e = i.next();
                    if ( e.eoo() )
                        break;
                    s << ", ";
                }
            s << " ]";
            break;
        }
        case DBRef: {
            OID *x = (OID *) (valuestr() + valuestrsize());
            if ( format == TenGen )
                s << "Dbref( ";
            else
                s << "{ \"$ref\" : ";
            s << '"' << valuestr() << "\", ";
            if ( format != TenGen )
                s << "\"$id\" : ";
            s << '"' << *x << "\" ";
            if ( format == TenGen )
                s << ')';
            else
                s << '}';
            break;
        }
        case jstOID:
            if ( format == TenGen ) {
                s << "ObjectId( ";
            } else {
                s << "{ \"$oid\" : ";
            }
            s << '"' << __oid() << '"';
            if ( format == TenGen ) {
                s << " )";
            } else {
                s << " }";
            }
            break;
        case BinData: {
            int len = *(int *)( value() );
            BinDataType type = BinDataType( *(char *)( (int *)( value() ) + 1 ) );
            s << "{ \"$binary\" : \"";
            char *start = ( char * )( value() ) + sizeof( int ) + 1;
            base64::encode( s , start , len );
            s << "\", \"$type\" : \"" << hex;
            s.width( 2 );
            s.fill( '0' );
            s << type << dec;
            s << "\" }";
            break;
        }
        case Date:
            if ( format == Strict )
                s << "{ \"$date\" : ";
            else
                s << "Date( ";
            s << date();
            if ( format == Strict )
                s << " }";
            else
                s << " )";
            break;
        case RegEx:
            if ( format == Strict )
                s << "{ \"$regex\" : \"";
            else
                s << "/";
            s << escape( regex() );
            if ( format == Strict )
                s << "\", \"$options\" : \"" << regexFlags() << "\" }";
            else {
                s << "/";
                // FIXME Worry about alpha order?
                for ( const char *f = regexFlags(); *f; ++f )
                    switch ( *f ) {
                    case 'g':
                    case 'i':
                    case 'm':
                        s << *f;
                    default:
                        break;
                    }
            }
            break;
        default:
            stringstream ss;
            ss << "Cannot create a properly formatted JSON string with "
            << "element: " << toString() << " of type: " << type();
            string message = ss.str();
            massert( message.c_str(), false );
        }
        return s.str();
    }

    int BSONElement::size( int maxLen ) const {
        if ( totalSize >= 0 )
            return totalSize;

        int remain = maxLen - fieldNameSize() - 1;

        int x = 0;
        switch ( type() ) {
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            break;
        case Bool:
            x = 1;
            break;
        case NumberInt:
            x = 4;
            break;
        case Timestamp:
        case Date:
        case NumberDouble:
        case NumberLong:
            x = 8;
            break;
        case jstOID:
            x = 12;
            break;
        case Symbol:
        case Code:
        case String:
            massert( "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = valuestrsize() + 4;
            break;
        case CodeWScope:
            massert( "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = objsize();
            break;

        case DBRef:
            massert( "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = valuestrsize() + 4 + 12;
            break;
        case Object:
        case Array:
            massert( "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = objsize();
            break;
        case BinData:
            massert( "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = valuestrsize() + 4 + 1/*subtype*/;
            break;
        case RegEx:
        {
            const char *p = value();
            int len1 = ( maxLen == -1 ) ? strlen( p ) : strnlen( p, remain );
            massert( "Invalid regex string", len1 != -1 );
            p = p + len1 + 1;
            int len2 = ( maxLen == -1 ) ? strlen( p ) : strnlen( p, remain - len1 - 1 );
            massert( "Invalid regex options string", len2 != -1 );
            x = len1 + 1 + len2 + 1;
        }
        break;
        default: {
            stringstream ss;
            ss << "BSONElement: bad type " << (int) type();
            massert(ss.str().c_str(),false);
        }
        }
        totalSize =  x + fieldNameSize() + 1; // BSONType

        return totalSize;
    }

    int BSONElement::getGtLtOp( int def ) const {
        const char *fn = fieldName();
        if ( fn[0] == '$' && fn[1] ) {
            if ( fn[2] == 't' ) {
                if ( fn[1] == 'g' ) {
                    if ( fn[3] == 0 ) return BSONObj::GT;
                    else if ( fn[3] == 'e' && fn[4] == 0 ) return BSONObj::GTE;
                }
                else if ( fn[1] == 'l' ) {
                    if ( fn[3] == 0 ) return BSONObj::LT;
                    else if ( fn[3] == 'e' && fn[4] == 0 ) return BSONObj::LTE;
                }
            }
            else if ( fn[1] == 'n' && fn[2] == 'e' && fn[3] == 0)
                return BSONObj::NE;
            else if ( fn[1] == 'm' && fn[2] == 'o' && fn[3] == 'd' && fn[4] == 0 )
                return BSONObj::opMOD;
            else if ( fn[1] == 't' && fn[2] == 'y' && fn[3] == 'p' && fn[4] == 'e' && fn[5] == 0 )
                return BSONObj::opTYPE;
            else if ( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0 )
                return BSONObj::opIN;
            else if ( fn[1] == 'n' && fn[2] == 'i' && fn[3] == 'n' && fn[4] == 0 )
                return BSONObj::NIN;
            else if ( fn[1] == 'a' && fn[2] == 'l' && fn[3] == 'l' && fn[4] == 0 )
                return BSONObj::opALL;
            else if ( fn[1] == 's' && fn[2] == 'i' && fn[3] == 'z' && fn[4] == 'e' && fn[5] == 0 )
                return BSONObj::opSIZE;
            else if ( fn[1] == 'e' && fn[2] == 'x' && fn[3] == 'i' && fn[4] == 's' && fn[5] == 't' && fn[6] == 's' && fn[7] == 0 )
                return BSONObj::opEXISTS;
            else if ( fn[1] == 'r' && fn[2] == 'e' && fn[3] == 'g' && fn[4] == 'e' && fn[5] == 'x' && fn[6] == 0 )
                return BSONObj::opREGEX;
            else if ( fn[1] == 'o' && fn[2] == 'p' && fn[3] == 't' && fn[4] == 'i' && fn[5] == 'o' && fn[6] == 'n' && fn[7] == 's' && fn[8] == 0 )
                return BSONObj::opOPTIONS;
        }
        return def;
    }

    /* wo = "well ordered" */
    int BSONElement::woCompare( const BSONElement &e,
                                bool considerFieldName ) const {
        int lt = (int) canonicalType();
        int rt = (int) e.canonicalType();
        int x = lt - rt;
        if( x != 0 && (!isNumber() || !e.isNumber()) )
            return x;
        if ( considerFieldName ) {
            x = strcmp(fieldName(), e.fieldName());
            if ( x != 0 )
                return x;
        }
        x = compareElementValues(*this, e);
        return x;
    }

    /* must be same type when called, unless both sides are #s
    */
    int compareElementValues(const BSONElement& l, const BSONElement& r) {
        int f;
        double x;

        switch ( l.type() ) {
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            f = l.canonicalType() - r.canonicalType();
            if ( f<0 ) return -1;
            return f==0 ? 0 : 1;
        case Bool:
            return *l.value() - *r.value();
        case Timestamp:
        case Date:
            if ( l.date() < r.date() )
                return -1;
            return l.date() == r.date() ? 0 : 1;
        case NumberLong:
            if( r.type() == NumberLong ) {
                long long L = l._numberLong();
                long long R = r._numberLong();
                if( L < R ) return -1;
                if( L == R ) return 0;
                return 1;
            }
            // else fall through
        case NumberInt:
        case NumberDouble: {
            double left = l.number();
            double right = r.number();
            bool lNan = !( left <= numeric_limits< double >::max() &&
                         left >= -numeric_limits< double >::max() );
            bool rNan = !( right <= numeric_limits< double >::max() &&
                         right >= -numeric_limits< double >::max() );
            if ( lNan ) {
                if ( rNan ) {
                    return 0;
                } else {
                    return -1;
                }
            } else if ( rNan ) {
                return 1;
            }
            x = left - right;
            if ( x < 0 ) return -1;
            return x == 0 ? 0 : 1;
            }
        case jstOID:
            return memcmp(l.value(), r.value(), 12);
        case Code:
        case Symbol:
        case String:
            /* todo: utf version */
            return strcmp(l.valuestr(), r.valuestr());
        case Object:
        case Array:
            return l.embeddedObject().woCompare( r.embeddedObject() );
        case DBRef:
        case BinData: {
            int lsz = l.valuesize();
            int rsz = r.valuesize();
            if ( lsz - rsz != 0 ) return lsz - rsz;
            return memcmp(l.value(), r.value(), lsz);
        }
        case RegEx:
        {
            int c = strcmp(l.regex(), r.regex());
            if ( c )
                return c;
            return strcmp(l.regexFlags(), r.regexFlags());
        }
        case CodeWScope : {
            f = l.canonicalType() - r.canonicalType();
            if ( f )
                return f;
            f = strcmp( l.codeWScopeCode() , r.codeWScopeCode() );
            if ( f )
                return f;
            f = strcmp( l.codeWScopeScopeData() , r.codeWScopeScopeData() );
            if ( f )
                return f;
            return 0;
        }
        default:
            out() << "compareElementValues: bad type " << (int) l.type() << endl;
            assert(false);
        }
        return -1;
    }

    /** returns a string that when used as a matcher, would match a super set of regex()
        returns "" for complex regular expressions
        used to optimize queries in some simple regex cases that start with '^'
    */
    string BSONElement::simpleRegex() const {

        string r = "";

        if ( *regexFlags() )
            return r;

        const char *i = regex();
        if ( *i != '^' )
            return r;
        ++i;

        // Empty string matches everything, won't limit our search.
        if ( !*i )
            return r;

        stringstream ss;
        for( ; *i; ++i ){
            char c = *i;
            if ( c == '*' || c == '?' ){
                r = ss.str();
                r = r.substr( 0 , r.size() - 1 );
                break;
            }
            else if ( *i == ' ' || (*i>='0'&&*i<='9') || (*i>='@'&&*i<='Z') || (*i>='a'&&*i<='z') ){
                ss << *i;
            }
            else {
                r = ss.str();
                break;
            }
        }

        if ( r.size() == 0 && *i == 0 )
            r = ss.str();

        return r;
    }

    void BSONElement::validate() const {
        switch( type() ) {
            case DBRef:
            case Code:
            case Symbol:
            case String:
                massert( "Invalid dbref/code/string/symbol size",
                        valuestrsize() > 0 &&
                        valuestrsize() - 1 == strnlen( valuestr(), valuestrsize() ) );
                break;
            case CodeWScope: {
                int totalSize = *( int * )( value() );
                massert( "Invalid CodeWScope size", totalSize >= 8 );
                int strSizeWNull = *( int * )( value() + 4 );
                massert( "Invalid CodeWScope string size", totalSize >= strSizeWNull + 4 + 4 );
                massert( "Invalid CodeWScope string size",
                        strSizeWNull > 0 &&
                        strSizeWNull - 1 == strnlen( codeWScopeCode(), strSizeWNull ) );
                massert( "Invalid CodeWScope size", totalSize >= strSizeWNull + 4 + 4 + 4 );
                int objSize = *( int * )( value() + 4 + 4 + strSizeWNull );
                massert( "Invalid CodeWScope object size", totalSize == 4 + 4 + strSizeWNull + objSize );
                // Subobject validation handled elsewhere.
            }
            case Object:
                // We expect Object size validation to be handled elsewhere.
            default:
                break;
        }
    }

    /* JSMatcher --------------------------------------*/

// If the element is something like:
//   a : { $gt : 3 }
// we append
//   a : 3
// else we just append the element.
//
    void appendElementHandlingGtLt(BSONObjBuilder& b, const BSONElement& e) {
        if ( e.type() == Object ) {
            BSONElement fe = e.embeddedObject().firstElement();
            const char *fn = fe.fieldName();
            if ( fn[0] == '$' && fn[1] && fn[2] == 't' ) {
                b.appendAs(fe, e.fieldName());
                return;
            }
        }
        b.append(e);
    }

    int getGtLtOp(const BSONElement& e) {
        if ( e.type() != Object )
            return BSONObj::Equality;

        BSONElement fe = e.embeddedObject().firstElement();
        return fe.getGtLtOp();
    }

    FieldCompareResult compareDottedFieldNames( const string& l , const string& r ){
        size_t lstart = 0;
        size_t rstart = 0;
        while ( 1 ){
            if ( lstart >= l.size() ){
                if ( rstart >= r.size() )
                    return SAME;
                return RIGHT_SUBFIELD;
            }
            if ( rstart >= r.size() )
                return LEFT_SUBFIELD;

            size_t a = l.find( '.' , lstart );
            size_t b = r.find( '.' , rstart );

            size_t lend = a == string::npos ? l.size() : a;
            size_t rend = b == string::npos ? r.size() : b;

            const string& c = l.substr( lstart , lend - lstart );
            const string& d = r.substr( rstart , rend - rstart );

            int x = c.compare( d );

            if ( x < 0 )
                return LEFT_BEFORE;
            if ( x > 0 )
                return RIGHT_BEFORE;

            lstart = lend + 1;
            rstart = rend + 1;
        }
    }

    /* BSONObj ------------------------------------------------------------*/

    BSONObj::EmptyObject BSONObj::emptyObject;

    string BSONObj::toString() const {
        if ( isEmpty() ) return "{}";

        stringstream s;
        s << "{ ";
        BSONObjIterator i(*this);
        bool first = true;
        while ( 1 ) {
            massert( "Object does not end with EOO", i.moreWithEOO() );
            BSONElement e = i.next( true );
            massert( "Invalid element size", e.size() > 0 );
            massert( "Element too large", e.size() < ( 1 << 30 ) );
            int offset = e.rawdata() - this->objdata();
            massert( "Element extends past end of object",
                    e.size() + offset <= this->objsize() );
            e.validate();
            bool end = ( e.size() + offset == this->objsize() );
            if ( e.eoo() ) {
                massert( "EOO Before end of object", end );
                break;
            }
            if ( first )
                first = false;
            else
                s << ", ";
            s << e.toString();
        }
        s << " }";
        return s.str();
    }

    string BSONObj::md5() const {
        md5digest d;
        md5_state_t st;
        md5_init(&st);
        md5_append( &st , (const md5_byte_t*)_objdata , objsize() );
        md5_finish(&st, d);
        return digestToString( d );
    }

    string BSONObj::jsonString( JsonStringFormat format ) const {

        if ( isEmpty() ) return "{}";

        stringstream s;
        s << "{ ";
        BSONObjIterator i(*this);
        BSONElement e = i.next();
        if ( !e.eoo() )
            while ( 1 ) {
                s << e.jsonString( format );
                e = i.next();
                if ( e.eoo() )
                    break;
                s << ", ";
            }
        s << " }";
        return s.str();
    }

// todo: can be a little faster if we don't use toString() here.
    bool BSONObj::valid() const {
        try {
            toString();
        }
        catch (...) {
            return false;
        }
        return true;
    }

    /* well ordered compare */
    int BSONObj::woCompare(const BSONObj &r, const BSONObj &idxKey,
                           bool considerFieldName) const {
        if ( isEmpty() )
            return r.isEmpty() ? 0 : -1;
        if ( r.isEmpty() )
            return 1;

        bool ordered = !idxKey.isEmpty();

        BSONObjIterator i(*this);
        BSONObjIterator j(r);
        BSONObjIterator k(idxKey);
        while ( 1 ) {
            // so far, equal...

            BSONElement l = i.next();
            BSONElement r = j.next();
            BSONElement o;
            if ( ordered )
                o = k.next();
            if ( l.eoo() )
                return r.eoo() ? 0 : -1;
            if ( r.eoo() )
                return 1;

            int x = l.woCompare( r, considerFieldName );
            if ( ordered && o.number() < 0 )
                x = -x;
            if ( x != 0 )
                return x;
        }
        return -1;
    }

    BSONObj staticNull = fromjson( "{'':null}" );

    /* well ordered compare */
    int BSONObj::woSortOrder(const BSONObj& other, const BSONObj& sortKey ) const{
        if ( isEmpty() )
            return other.isEmpty() ? 0 : -1;
        if ( other.isEmpty() )
            return 1;

        uassert( "woSortOrder needs a non-empty sortKey" , ! sortKey.isEmpty() );

        BSONObjIterator i(sortKey);
        while ( 1 ){
            BSONElement f = i.next();
            if ( f.eoo() )
                return 0;

            BSONElement l = getField( f.fieldName() );
            if ( l.eoo() )
                l = staticNull.firstElement();
            BSONElement r = other.getField( f.fieldName() );
            if ( r.eoo() )
                r = staticNull.firstElement();

            int x = l.woCompare( r, false );
            if ( f.number() < 0 )
                x = -x;
            if ( x != 0 )
                return x;
        }
        return -1;
    }


    BSONElement BSONObj::getField(const char *name) const {
        BSONObjIterator i(*this);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( strcmp(e.fieldName(), name) == 0 )
                return e;
        }
        return nullElement;
    }

    /* return has eoo() true if no match
       supports "." notation to reach into embedded objects
    */
    BSONElement BSONObj::getFieldDotted(const char *name) const {
        BSONElement e = getField( name );
        if ( e.eoo() ) {
            const char *p = strchr(name, '.');
            if ( p ) {
                string left(name, p-name);
                BSONObj sub = getObjectField(left.c_str());
                return sub.isEmpty() ? nullElement : sub.getFieldDotted(p+1);
            }
        }

        return e;
    }

    /* jul09 : 'deep' and this function will be going away in the future - kept only for backward compatibility of datafiles for now. */
    void trueDat( bool *deep ) {
        if( deep )
            *deep = true;
    }

    void BSONObj::getFieldsDotted(const char *name, BSONElementSet &ret, bool *deep ) const {
        BSONElement e = getField( name );
        if ( e.eoo() ) {
            const char *p = strchr(name, '.');
            if ( p ) {
                string left(name, p-name);
                BSONElement e = getField( left );
                if ( e.type() == Array ) {
                    trueDat( deep );
                    BSONObjIterator i( e.embeddedObject() );
                    while( i.moreWithEOO() ) {
                        BSONElement f = i.next();
                        if ( f.eoo() )
                            break;
                        if ( f.type() == Object )
                            f.embeddedObject().getFieldsDotted(p+1, ret);
                    }
                } else if ( e.type() == Object ) {
                    e.embeddedObject().getFieldsDotted(p+1, ret);
                }
            }
        } else {
            if ( e.type() == Array ) {
                trueDat( deep );
                BSONObjIterator i( e.embeddedObject() );
                while( i.moreWithEOO() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    ret.insert( f );
                }
            } else {
                ret.insert( e );
            }
        }
        if ( ret.empty() && deep )
            *deep = false;
    }

    BSONElement BSONObj::getFieldDottedOrArray(const char *&name) const {
        const char *p = strchr(name, '.');
        string left;
        if ( p ) {
            left = string(name, p-name);
            name = p + 1;
        } else {
            left = string(name);
            name = name + strlen(name);
        }
        BSONElement sub = getField(left.c_str());
        if ( sub.eoo() )
            return nullElement;
        else if ( sub.type() == Array || strlen( name ) == 0 )
            return sub;
        else if ( sub.type() == Object )
            return sub.embeddedObject().getFieldDottedOrArray( name );
        else
            return nullElement;
    }

    /* makes a new BSONObj with the fields specified in pattern.
       fields returned in the order they appear in pattern.
       if any field missing from the original object, that field
       in the key will be null.

       n^2 implementation bad if pattern and object have lots
       of fields - normally pattern doesn't so should be fine.
    */
    BSONObj BSONObj::extractFieldsDotted(BSONObj pattern, BSONObjBuilder& b, const char *&nameWithinArray) const {
        nameWithinArray = "";
        BSONObjIterator i(pattern);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const char *name = e.fieldName();
            BSONElement x = getFieldDottedOrArray( name );
            if ( x.eoo() ) {
                b.appendNull( "" );
                continue;
            } else if ( x.type() == Array ) {
                // NOTE: Currently set based on last array discovered.
                nameWithinArray = name;
            }
            b.appendAs(x, "");
        }
        return b.done();
    }

    /**
     sets element field names to empty string
     If a field in pattern is missing, it is omitted from the returned
     object.
     */
    BSONObj BSONObj::extractFieldsUnDotted(BSONObj pattern) const {
        BSONObjBuilder b;
        BSONObjIterator i(pattern);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = getField(e.fieldName());
            if ( !x.eoo() )
                b.appendAs(x, "");
        }
        return b.obj();
    }

    BSONObj BSONObj::extractFields(const BSONObj& pattern , bool fillWithNull ) const {
        BSONObjBuilder b(32); // scanandorder.h can make a zillion of these, so we start the allocation very small
        BSONObjIterator i(pattern);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = getFieldDotted(e.fieldName());
            if ( ! x.eoo() )
                b.appendAs( x, e.fieldName() );
            else if ( fillWithNull )
                b.appendNull( e.fieldName() );
        }
        return b.obj();
    }

    BSONObj BSONObj::filterFieldsUndotted( const BSONObj &filter, bool inFilter ) const {
        BSONObjBuilder b;
        BSONObjIterator i( *this );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = filter.getField( e.fieldName() );
            if ( ( x.eoo() && !inFilter ) ||
                ( !x.eoo() && inFilter ) )
                b.append( e );
        }
        return b.obj();
    }

    BSONElement BSONObj::getFieldUsingIndexNames(const char *fieldName, const BSONObj &indexKey) const {
        BSONObjIterator i( indexKey );
        int j = 0;
        while( i.moreWithEOO() ) {
            BSONElement f = i.next();
            if ( f.eoo() )
                return BSONElement();
            if ( strcmp( f.fieldName(), fieldName ) == 0 )
                break;
            ++j;
        }
        BSONObjIterator k( *this );
        while( k.moreWithEOO() ) {
            BSONElement g = k.next();
            if ( g.eoo() )
                return BSONElement();
            if ( j == 0 ) {
                return g;
            }
            --j;
        }
        return BSONElement();
    }

    int BSONObj::getIntField(const char *name) const {
        BSONElement e = getField(name);
        return e.isNumber() ? (int) e.number() : INT_MIN;
    }

    bool BSONObj::getBoolField(const char *name) const {
        BSONElement e = getField(name);
        return e.type() == Bool ? e.boolean() : false;
    }

    const char * BSONObj::getStringField(const char *name) const {
        BSONElement e = getField(name);
        return e.type() == String ? e.valuestr() : "";
    }

    BSONObj BSONObj::getObjectField(const char *name) const {
        BSONElement e = getField(name);
        BSONType t = e.type();
        return t == Object || t == Array ? e.embeddedObject() : BSONObj();
    }

    int BSONObj::nFields() const {
        int n = 0;
        BSONObjIterator i(*this);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            n++;
        }
        return n;
    }

    /* grab names of all the fields in this object */
    int BSONObj::getFieldNames(set<string>& fields) const {
        int n = 0;
        BSONObjIterator i(*this);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            fields.insert(e.fieldName());
            n++;
        }
        return n;
    }

    /* note: addFields always adds _id even if not specified
       returns n added not counting _id unless requested.
    */
    int BSONObj::addFields(BSONObj& from, set<string>& fields) {
        assert( isEmpty() && !isOwned() ); /* partial implementation for now... */

        BSONObjBuilder b;

        int N = fields.size();
        int n = 0;
        BSONObjIterator i(from);
        bool gotId = false;
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            const char *fname = e.fieldName();
            if ( fields.count(fname) ) {
                b.append(e);
                ++n;
                gotId = gotId || strcmp(fname, "_id")==0;
                if ( n == N && gotId )
                    break;
            } else if ( strcmp(fname, "_id")==0 ) {
                b.append(e);
                gotId = true;
                if ( n == N && gotId )
                    break;
            }
        }

        if ( n ) {
            int len;
            init( b.decouple(len), true );
        }

        return n;
    }

    BSONObj BSONObj::clientReadable() const {
        BSONObjBuilder b;
        BSONObjIterator i( *this );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            switch( e.type() ) {
                case MinKey: {
                    BSONObjBuilder m;
                    m.append( "$minElement", 1 );
                    b.append( e.fieldName(), m.done() );
                    break;
                }
                case MaxKey: {
                    BSONObjBuilder m;
                    m.append( "$maxElement", 1 );
                    b.append( e.fieldName(), m.done() );
                    break;
                }
                default:
                    b.append( e );
            }
        }
        return b.obj();
    }

    BSONObj BSONObj::replaceFieldNames( const BSONObj &names ) const {
        BSONObjBuilder b;
        BSONObjIterator i( *this );
        BSONObjIterator j( names );
        BSONElement f = j.moreWithEOO() ? j.next() : BSONObj().firstElement();
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( !f.eoo() ) {
                b.appendAs( e, f.fieldName() );
                f = j.next();
            } else {
                b.append( e );
            }
        }
        return b.obj();
    }

    string BSONObj::hexDump() const {
        stringstream ss;
        const char *d = objdata();
        int size = objsize();
        for( int i = 0; i < size; ++i ) {
            ss.width( 2 );
            ss.fill( '0' );
            ss << hex << (unsigned)(unsigned char)( d[ i ] ) << dec;
            if ( ( d[ i ] >= '0' && d[ i ] <= '9' ) || ( d[ i ] >= 'A' && d[ i ] <= 'z' ) )
                ss << '\'' << d[ i ] << '\'';
            if ( i != size - 1 )
                ss << ' ';
        }
        return ss.str();
    }

    ostream& operator<<( ostream &s, const BSONObj &o ) {
        return s << o.toString();
    }

    ostream& operator<<( ostream &s, const BSONElement &e ) {
        return s << e.toString();
    }

    void nested2dotted(BSONObjBuilder& b, const BSONObj& obj, const string& base){
        BSONObjIterator it(obj);
        while (it.more()){
            BSONElement e = it.next();
            if (e.type() == Object){
                string newbase = base + e.fieldName() + ".";
                nested2dotted(b, e.embeddedObject(), newbase);
            }else{
                string newbase = base + e.fieldName();
                b.appendAs(e, newbase.c_str());
            }
        }
    }

    void dotted2nested(BSONObjBuilder& b, const BSONObj& obj){
        //use map to sort fields
        BSONMap sorted = bson2map(obj);
        EmbeddedBuilder eb(&b);
        for(BSONMap::const_iterator it=sorted.begin(); it!=sorted.end(); ++it){
            eb.appendAs(it->second, it->first);
        }
        eb.done();
    }

    /*-- test things ----------------------------------------------------*/

#pragma pack(1)
    struct MaxKeyData {
        MaxKeyData() {
            totsize=7;
            maxkey=MaxKey;
            name=0;
            eoo=EOO;
        }
        int totsize;
        char maxkey;
        char name;
        char eoo;
    } maxkeydata;
    BSONObj maxKey((const char *) &maxkeydata);

    struct MinKeyData {
        MinKeyData() {
            totsize=7;
            minkey=MinKey;
            name=0;
            eoo=EOO;
        }
        int totsize;
        char minkey;
        char name;
        char eoo;
    } minkeydata;
    BSONObj minKey((const char *) &minkeydata);

    struct JSObj0 {
        JSObj0() {
            totsize = 5;
            eoo = EOO;
        }
        int totsize;
        char eoo;
    } js0;
#pragma pack()

    BSONElement::BSONElement() {
        data = &js0.eoo;
        fieldNameSize_ = 0;
        totalSize = 1;
    }

    struct BsonUnitTest : public UnitTest {
        void testRegex() {

            BSONObjBuilder b;
            b.appendRegex("x", "foo");
            BSONObj o = b.done();

            BSONObjBuilder c;
            c.appendRegex("x", "goo");
            BSONObj p = c.done();

            assert( !o.woEqual( p ) );
            assert( o.woCompare( p ) < 0 );

            {
                BSONObjBuilder b;
                b.appendRegex("r", "^foo");
                BSONObj o = b.done();
                assert( o.firstElement().simpleRegex() == "foo" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^f?oo");
                BSONObj o = b.done();
                assert( o.firstElement().simpleRegex() == "" );
            }
            {
                BSONObjBuilder b;
                b.appendRegex("r", "^fz?oo");
                BSONObj o = b.done();
                assert( o.firstElement().simpleRegex() == "f" );
            }
        }
        void testoid() {
            OID id;
            id.init();
            //            sleepsecs(3);

            OID b;
            // goes with sleep above...
            // b.init();
            // assert( memcmp(id.getData(), b.getData(), 12) < 0 );

            b.init( id.str() );
            assert( b == id );
        }

        void testbounds(){
            BSONObj l , r;
            {
                BSONObjBuilder b;
                b.append( "x" , numeric_limits<long long>::max() );
                l = b.obj();
            }
            {
                BSONObjBuilder b;
                b.append( "x" , numeric_limits<double>::max() );
                r = b.obj();
            }
            assert( l.woCompare( r ) < 0 );
            assert( r.woCompare( l ) > 0 );
            {
                BSONObjBuilder b;
                b.append( "x" , numeric_limits<int>::max() );
                l = b.obj();
            }
            assert( l.woCompare( r ) < 0 );
            assert( r.woCompare( l ) > 0 );
        }

        void testorder(){
            {
                BSONObj x,y,z;
                { BSONObjBuilder b; b.append( "x" , (long long)2 ); x = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (int)3 ); y = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (long long)4 ); z = b.obj(); }
                assert( x.woCompare( y ) < 0 );
                assert( x.woCompare( z ) < 0 );
                assert( y.woCompare( x ) > 0 );
                assert( z.woCompare( x ) > 0 );
                assert( y.woCompare( z ) < 0 );
                assert( z.woCompare( y ) > 0 );
            }

            {
                BSONObj ll,d,i,n,u;
                { BSONObjBuilder b; b.append( "x" , (long long)2 ); ll = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (double)2 ); d = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (int)2 ); i = b.obj(); }
                { BSONObjBuilder b; b.appendNull( "x" ); n = b.obj(); }
                { BSONObjBuilder b; u = b.obj(); }

                assert( ll.woCompare( u ) == d.woCompare( u ) );
                assert( ll.woCompare( u ) == i.woCompare( u ) );
                BSONObj k = BSON( "x" << 1 );
                assert( ll.woCompare( u , k ) == d.woCompare( u , k ) );
                assert( ll.woCompare( u , k ) == i.woCompare( u , k ) );

                assert( u.woCompare( ll ) == u.woCompare( d ) );
                assert( u.woCompare( ll ) == u.woCompare( i ) );
                assert( u.woCompare( ll , k ) == u.woCompare( d , k ) );
                assert( u.woCompare( ll , k ) == u.woCompare( d , k ) );

                assert( i.woCompare( n ) == d.woCompare( n ) );

                assert( ll.woCompare( n ) == d.woCompare( n ) );
                assert( ll.woCompare( n ) == i.woCompare( n ) );
                assert( ll.woCompare( n , k ) == d.woCompare( n , k ) );
                assert( ll.woCompare( n , k ) == i.woCompare( n , k ) );

                assert( n.woCompare( ll ) == n.woCompare( d ) );
                assert( n.woCompare( ll ) == n.woCompare( i ) );
                assert( n.woCompare( ll , k ) == n.woCompare( d , k ) );
                assert( n.woCompare( ll , k ) == n.woCompare( d , k ) );
            }

            {
                BSONObj l,r;
                { BSONObjBuilder b; b.append( "x" , "eliot" ); l = b.obj(); }
                { BSONObjBuilder b; b.appendSymbol( "x" , "eliot" ); r = b.obj(); }
                assert( l.woCompare( r ) == 0 );
                assert( r.woCompare( l ) == 0 );
            }
        }

        void run() {
            testRegex();
            BSONObjBuilder A,B,C;
            A.append("x", 2);
            B.append("x", 2.0);
            C.append("x", 2.1);
            BSONObj a = A.done();
            BSONObj b = B.done();
            BSONObj c = C.done();
            assert( !a.woEqual( b ) ); // comments on operator==
            int cmp = a.woCompare(b);
            assert( cmp == 0 );
            cmp = a.woCompare(c);
            assert( cmp < 0 );
            testoid();
            testbounds();
            testorder();
        }
    } bson_unittest;

/*
    BSONObjBuilder& BSONObjBuilderValueStream::operator<<( const char * value ) {
        _builder->append( _fieldName , value );
        return *_builder;
    }

    BSONObjBuilder& BSONObjBuilderValueStream::operator<<( const int value ) {
        _builder->append( _fieldName , value );
        return *_builder;
    }

    BSONObjBuilder& BSONObjBuilderValueStream::operator<<( const double value ) {
        _builder->append( _fieldName , value );
        return *_builder;
    }
*/

    unsigned OID::_machine = (unsigned) security.getNonceInitSafe();
    void OID::newState(){
        // using fresh Security object to avoid buffered devrandom
        _machine = (unsigned) Security().getNonce();
    }
    
    void OID::init() {
        static WrappingInt inc = (unsigned) security.getNonce();
        unsigned t = (unsigned) time(0);
        char *T = (char *) &t;
        data[0] = T[3];
        data[1] = T[2];
        data[2] = T[1];
        data[3] = T[0];

        (unsigned&) data[4] = _machine;

        int new_inc = inc.atomicIncrement();
        T = (char *) &new_inc;
        char * raw = (char*)&b;
        raw[0] = T[3];
        raw[1] = T[2];
        raw[2] = T[1];
        raw[3] = T[0];
    }

    void OID::init( string s ){
        assert( s.size() == 24 );
        const char *p = s.c_str();
        char buf[3];
        buf[2] = 0;
        for( int i = 0; i < 12; i++ ) {
            buf[0] = p[0];
            buf[1] = p[1];
            p += 2;
            stringstream ss(buf);
            unsigned z;
            ss >> hex >> z;
            data[i] = z;
        }

/*
        string as = s.substr( 0 , 16 );
        string bs = s.substr( 16 );

        stringstream ssa(as);
        ssa >> hex >> a;

        stringstream ssb(bs);
        ssb >> hex >> b;
*/
    }

    Labeler::Label GT( "$gt" );
    Labeler::Label GTE( "$gte" );
    Labeler::Label LT( "$lt" );
    Labeler::Label LTE( "$lte" );
    Labeler::Label NE( "$ne" );
    Labeler::Label SIZE( "$size" );

    void BSONElementManipulator::initTimestamp() {
        massert( "Expected CurrentTime type", element_.type() == Timestamp );
        unsigned long long &timestamp = *( reinterpret_cast< unsigned long long* >( value() ) );
        if ( timestamp == 0 )
            timestamp = OpTime::now().asDate();
    }


    void BSONObjBuilder::appendMinForType( const string& field , int t ){
        switch ( t ){
        case MinKey: appendMinKey( field.c_str() ); return;
        case MaxKey: appendMinKey( field.c_str() ); return;
        case NumberInt:
        case NumberDouble:
        case NumberLong:
            append( field.c_str() , - numeric_limits<double>::max() ); return;
        case jstOID:
            {
                OID o;
                memset(&o, 0, sizeof(o));
                appendOID( field.c_str() , &o);
                return;
            }
        case Bool: appendBool( field.c_str() , false); return;
        case Date: appendDate( field.c_str() , 0); return;
        case jstNULL: appendNull( field.c_str() ); return;
        case Symbol:
        case String: append( field.c_str() , "" ); return;
        case Object: append( field.c_str() , BSONObj() ); return;
        case Array:
            appendArray( field.c_str() , BSONObj() ); return;
        case BinData:
            appendBinData( field.c_str() , 0 , Function , (const char *) 0 ); return;
        case Undefined:
            appendUndefined( field.c_str() ); return;
        case RegEx: appendRegex( field.c_str() , "" ); return;
        case DBRef:
            {
                OID o;
                memset(&o, 0, sizeof(o));
                appendDBRef( field.c_str() , "" , o );
                return;
            }
        case Code: appendCode( field.c_str() , "" ); return;
        case CodeWScope: appendCodeWScope( field.c_str() , "" , BSONObj() ); return;
        case Timestamp: appendTimestamp( field.c_str() , 0); return;

        };
        log() << "type not support for appendMinElementForType: " << t << endl;
        uassert( "type not supported for appendMinElementForType" , false );
    }

    void BSONObjBuilder::appendMaxForType( const string& field , int t ){
        switch ( t ){
        case MinKey: appendMaxKey( field.c_str() );  break;
        case MaxKey: appendMaxKey( field.c_str() ); break;
        case NumberInt:
        case NumberDouble:
        case NumberLong:
            append( field.c_str() , numeric_limits<double>::max() );
            break;
        case BinData:
            appendMinForType( field , jstOID );
            break;
        case jstOID:
            {
                OID o;
                memset(&o, 0xFF, sizeof(o));
                appendOID( field.c_str() , &o);
                break;
            }
        case Undefined:
        case jstNULL:
            appendMinForType( field , NumberInt );
        case Bool: appendBool( field.c_str() , true); break;
        case Date: appendDate( field.c_str() , 0xFFFFFFFFFFFFFFFFLL ); break;
        case Symbol:
        case String: append( field.c_str() , BSONObj() ); break;
        case Code:
        case CodeWScope:
            appendCodeWScope( field.c_str() , "ZZZ" , BSONObj() ); break;
        case Timestamp:
            appendTimestamp( field.c_str() , numeric_limits<unsigned long long>::max() ); break;
        default:
            appendMinForType( field , t + 1 );
        }
    }

    const string BSONObjBuilder::numStrs[] = {
         "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",
        "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
        "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
        "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
        "40", "41", "42", "43", "44", "45", "46", "47", "48", "49",
        "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
        "60", "61", "62", "63", "64", "65", "66", "67", "68", "69",
        "70", "71", "72", "73", "74", "75", "76", "77", "78", "79",
        "80", "81", "82", "83", "84", "85", "86", "87", "88", "89",
        "90", "91", "92", "93", "94", "95", "96", "97", "98", "99",
    };

    bool BSONObjBuilder::appendAsNumber( const string& fieldName , const string& data ){
        if ( data.size() == 0 )
            return false;
        
        unsigned int pos=0;
        if ( data[0] == '-' )
            pos++;
        
        bool hasDec = false;
        
        for ( ; pos<data.size(); pos++ ){
            if ( isdigit(data[pos]) )
                continue;

            if ( data[pos] == '.' ){
                if ( hasDec )
                    return false;
                hasDec = true;
                continue;
            }
            
            return false;
        }
        
        if ( hasDec ){
            double d = atof( data.c_str() );
            append( fieldName.c_str() , d );
            return true;
        }
        
        if ( data.size() < 8 ){
            append( fieldName , atoi( data.c_str() ) );
            return true;
        }
        
        try {
            long long num = boost::lexical_cast<long long>( data );
            append( fieldName , num );
            return true;
        }
        catch(bad_lexical_cast &){
            return false;
        }

    }

} // namespace mongo
