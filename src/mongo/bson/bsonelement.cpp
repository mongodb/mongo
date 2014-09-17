/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/bson/bsonelement.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/base64.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {  
    namespace str = mongoutils::str;

    string BSONElement::jsonString( JsonStringFormat format, bool includeFieldNames, int pretty ) const {
        int sign;

        std::stringstream s;
        if ( includeFieldNames )
            s << '"' << escape( fieldName() ) << "\" : ";
        switch ( type() ) {
        case mongo::String:
        case Symbol:
            s << '"' << escape( string(valuestr(), valuestrsize()-1) ) << '"';
            break;
        case NumberLong:
            if (format == TenGen) {
                s << "NumberLong(" << _numberLong() << ")";
            }
            else {
                s << "{ \"$numberLong\" : \"" << _numberLong() << "\" }";
            }
            break;
        case NumberInt:
            if(format == JS) {
                s << "NumberInt(" << _numberInt() << ")";
                break;
            }
        case NumberDouble:
            if ( number() >= -std::numeric_limits< double >::max() &&
                 number() <= std::numeric_limits< double >::max() ) {
                s.precision( 16 );
                s << number();
            }
            // This is not valid JSON, but according to RFC-4627, "Numeric values that cannot be
            // represented as sequences of digits (such as Infinity and NaN) are not permitted." so
            // we are accepting the fact that if we have such values we cannot output valid JSON.
            else if ( mongo::isNaN(number()) ) {
                s << "NaN";
            }
            else if ( mongo::isInf(number(), &sign) ) {
                s << ( sign == 1 ? "Infinity" : "-Infinity");
            }
            else {
                StringBuilder ss;
                ss << "Number " << number() << " cannot be represented in JSON";
                string message = ss.str();
                massert( 10311 ,  message.c_str(), false );
            }
            break;
        case mongo::Bool:
            s << ( boolean() ? "true" : "false" );
            break;
        case jstNULL:
            s << "null";
            break;
        case Undefined:
            if ( format == Strict ) {
                s << "{ \"$undefined\" : true }";
            }
            else {
                s << "undefined";
            }
            break;
        case Object:
            s << embeddedObject().jsonString( format, pretty );
            break;
        case mongo::Array: {
            if ( embeddedObject().isEmpty() ) {
                s << "[]";
                break;
            }
            s << "[ ";
            BSONObjIterator i( embeddedObject() );
            BSONElement e = i.next();
            if ( !e.eoo() ) {
                int count = 0;
                while ( 1 ) {
                    if( pretty ) {
                        s << '\n';
                        for( int x = 0; x < pretty; x++ )
                            s << "  ";
                    }

                    if (strtol(e.fieldName(), 0, 10) > count) {
                        s << "undefined";
                    }
                    else {
                        s << e.jsonString( format, false, pretty?pretty+1:0 );
                        e = i.next();
                    }
                    count++;
                    if ( e.eoo() )
                        break;
                    s << ", ";
                }
            }
            s << " ]";
            break;
        }
        case DBRef: {
            mongo::OID *x = (mongo::OID *) (valuestr() + valuestrsize());
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
            }
            else {
                s << "{ \"$oid\" : ";
            }
            s << '"' << __oid() << '"';
            if ( format == TenGen ) {
                s << " )";
            }
            else {
                s << " }";
            }
            break;
        case BinData: {
            const int len = *( reinterpret_cast<const int*>( value() ) );
            BinDataType type = BinDataType( *( reinterpret_cast<const unsigned char*>( value() ) +
                                               sizeof( int ) ) );
            s << "{ \"$binary\" : \"";
            const char *start = reinterpret_cast<const char*>( value() ) + sizeof( int ) + 1;
            base64::encode( s , start , len );
            s << "\", \"$type\" : \"" << hex;
            s.width( 2 );
            s.fill( '0' );
            s << type << dec;
            s << "\" }";
            break;
        }
        case mongo::Date:
            if (format == Strict) {
                Date_t d = date();
                s << "{ \"$date\" : ";
                // The two cases in which we cannot convert Date_t::millis to an ISO Date string are
                // when the date is too large to format (SERVER-13760), and when the date is before
                // the epoch (SERVER-11273).  Since Date_t internally stores millis as an unsigned
                // long long, despite the fact that it is logically signed (SERVER-8573), this check
                // handles both the case where Date_t::millis is too large, and the case where
                // Date_t::millis is negative (before the epoch).
                if (d.isFormatable()) {
                    s << "\"" << dateToISOStringLocal(date()) << "\"";
                }
                else {
                    s << "{ \"$numberLong\" : \"" << static_cast<long long>(d.millis) << "\" }";
                }
                s << " }";
            }
            else {
                s << "Date( ";
                if (pretty) {
                    Date_t d = date();
                    // The two cases in which we cannot convert Date_t::millis to an ISO Date string
                    // are when the date is too large to format (SERVER-13760), and when the date is
                    // before the epoch (SERVER-11273).  Since Date_t internally stores millis as an
                    // unsigned long long, despite the fact that it is logically signed
                    // (SERVER-8573), this check handles both the case where Date_t::millis is too
                    // large, and the case where Date_t::millis is negative (before the epoch).
                    if (d.isFormatable()) {
                        s << "\"" << dateToISOStringLocal(date()) << "\"";
                    }
                    else {
                        // FIXME: This is not parseable by the shell, since it may not fit in a
                        // float
                        s << d.millis;
                    }
                }
                else {
                    s << date().asInt64();
                }
                s << " )";
            }
            break;
        case RegEx:
            if ( format == Strict ) {
                s << "{ \"$regex\" : \"" << escape( regex() );
                s << "\", \"$options\" : \"" << regexFlags() << "\" }";
            }
            else {
                s << "/" << escape( regex() , true ) << "/";
                // FIXME Worry about alpha order?
                for ( const char *f = regexFlags(); *f; ++f ) {
                    switch ( *f ) {
                    case 'g':
                    case 'i':
                    case 'm':
                        s << *f;
                    default:
                        break;
                    }
                }
            }
            break;

        case CodeWScope: {
            BSONObj scope = codeWScopeObject();
            if ( ! scope.isEmpty() ) {
                s << "{ \"$code\" : \"" << escape(_asCode()) << "\" , "
                  << "\"$scope\" : " << scope.jsonString() << " }";
                break;
            }
        }

        case Code:
            s << "\"" << escape(_asCode()) << "\"";
            break;

        case Timestamp:
            if ( format == TenGen ) {
                s << "Timestamp( " << ( timestampTime() / 1000 ) << ", " << timestampInc() << " )";
            }
            else {
                s << "{ \"$timestamp\" : { \"t\" : " << ( timestampTime() / 1000 ) << ", \"i\" : " << timestampInc() << " } }";
            }
            break;

        case MinKey:
            s << "{ \"$minKey\" : 1 }";
            break;

        case MaxKey:
            s << "{ \"$maxKey\" : 1 }";
            break;

        default:
            StringBuilder ss;
            ss << "Cannot create a properly formatted JSON string with "
               << "element: " << toString() << " of type: " << type();
            string message = ss.str();
            massert( 10312 ,  message.c_str(), false );
        }
        return s.str();
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
            else if ( fn[1] == 'n' && fn[2] == 'e' ) {
                if ( fn[3] == 0 )
                    return BSONObj::NE;
                if ( fn[3] == 'a' && fn[4] == 'r') // matches anything with $near prefix
                    return BSONObj::opNEAR;
            }
            else if ( fn[1] == 'm' ) {
                if ( fn[2] == 'o' && fn[3] == 'd' && fn[4] == 0 )
                    return BSONObj::opMOD;
                if ( fn[2] == 'a' && fn[3] == 'x' && fn[4] == 'D' && fn[5] == 'i' && fn[6] == 's' && fn[7] == 't' && fn[8] == 'a' && fn[9] == 'n' && fn[10] == 'c' && fn[11] == 'e' && fn[12] == 0 )
                    return BSONObj::opMAX_DISTANCE;
            }
            else if ( fn[1] == 't' && fn[2] == 'y' && fn[3] == 'p' && fn[4] == 'e' && fn[5] == 0 )
                return BSONObj::opTYPE;
            else if ( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0) {
                return BSONObj::opIN;
            } else if ( fn[1] == 'n' && fn[2] == 'i' && fn[3] == 'n' && fn[4] == 0 )
                return BSONObj::NIN;
            else if ( fn[1] == 'a' && fn[2] == 'l' && fn[3] == 'l' && fn[4] == 0 )
                return BSONObj::opALL;
            else if ( fn[1] == 's' && fn[2] == 'i' && fn[3] == 'z' && fn[4] == 'e' && fn[5] == 0 )
                return BSONObj::opSIZE;
            else if ( fn[1] == 'e' ) {
                if ( fn[2] == 'x' && fn[3] == 'i' && fn[4] == 's' && fn[5] == 't' && fn[6] == 's' && fn[7] == 0 )
                    return BSONObj::opEXISTS;
                if ( fn[2] == 'l' && fn[3] == 'e' && fn[4] == 'm' && fn[5] == 'M' && fn[6] == 'a' && fn[7] == 't' && fn[8] == 'c' && fn[9] == 'h' && fn[10] == 0 )
                    return BSONObj::opELEM_MATCH;
            }
            else if ( fn[1] == 'r' && fn[2] == 'e' && fn[3] == 'g' && fn[4] == 'e' && fn[5] == 'x' && fn[6] == 0 )
                return BSONObj::opREGEX;
            else if ( fn[1] == 'o' && fn[2] == 'p' && fn[3] == 't' && fn[4] == 'i' && fn[5] == 'o' && fn[6] == 'n' && fn[7] == 's' && fn[8] == 0 )
                return BSONObj::opOPTIONS;
            else if ( fn[1] == 'w' && fn[2] == 'i' && fn[3] == 't' && fn[4] == 'h' && fn[5] == 'i' && fn[6] == 'n' && fn[7] == 0 )
                return BSONObj::opWITHIN;
            else if (str::equals(fn + 1, "geoIntersects"))
                return BSONObj::opGEO_INTERSECTS;
            else if (str::equals(fn + 1, "geoNear"))
                return BSONObj::opNEAR;
            else if (str::equals(fn + 1, "geoWithin"))
                return BSONObj::opWITHIN;
        }
        return def;
    }

    /** transform a BSON array into a vector of BSONElements.
        we match array # positions with their vector position, and ignore
        any fields with non-numeric field names.
        */
    std::vector<BSONElement> BSONElement::Array() const {
        chk(mongo::Array);
        std::vector<BSONElement> v;
        BSONObjIterator i(Obj());
        while( i.more() ) {
            BSONElement e = i.next();
            const char *f = e.fieldName();

            unsigned u;
            Status status = parseNumberFromString( f, &u );
            if ( status.isOK() ) {
                verify( u < 1000000 );
                if( u >= v.size() )
                    v.resize(u+1);
                v[u] = e;
            }
            else {
                // ignore?
            }
        }
        return v;
    }
    
    /* wo = "well ordered" 
       note: (mongodb related) : this can only change in behavior when index version # changes
    */
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


    BSONObj BSONElement::embeddedObjectUserCheck() const {
        if ( MONGO_likely(isABSONObj()) )
            return BSONObj(value());
        std::stringstream ss;
        ss << "invalid parameter: expected an object (" << fieldName() << ")";
        uasserted( 10065 , ss.str() );
        return BSONObj(); // never reachable
    }

    BSONObj BSONElement::embeddedObject() const {
        verify( isABSONObj() );
        return BSONObj(value());
    }

    BSONObj BSONElement::codeWScopeObject() const {
        verify( type() == CodeWScope );
        int strSizeWNull = ConstDataView(value() + 4).readLE<int>();
        return BSONObj( value() + 4 + 4 + strSizeWNull );
    }

    // wrap this element up as a singleton object.
    BSONObj BSONElement::wrap() const {
        BSONObjBuilder b(size()+6);
        b.append(*this);
        return b.obj();
    }

    BSONObj BSONElement::wrap( const StringData& newName ) const {
        BSONObjBuilder b(size() + 6 + newName.size());
        b.appendAs(*this,newName);
        return b.obj();
    }

    void BSONElement::Val(BSONObj& v) const { 
        v = Obj(); 
    }

    BSONObj BSONElement::Obj() const { 
        return embeddedObjectUserCheck(); 
    }

    BSONElement BSONElement::operator[] (const std::string& field) const {
        BSONObj o = Obj();
        return o[field];
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
        case mongo::Bool:
            x = 1;
            break;
        case NumberInt:
            x = 4;
            break;
        case Timestamp:
        case mongo::Date:
        case NumberDouble:
        case NumberLong:
            x = 8;
            break;
        case jstOID:
            x = 12;
            break;
        case Symbol:
        case Code:
        case mongo::String:
            massert( 10313 ,  "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = valuestrsize() + 4;
            break;
        case CodeWScope:
            massert( 10314 ,  "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = objsize();
            break;

        case DBRef:
            massert( 10315 ,  "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = valuestrsize() + 4 + 12;
            break;
        case Object:
        case mongo::Array:
            massert( 10316 ,  "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = objsize();
            break;
        case BinData:
            massert( 10317 ,  "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3 );
            x = valuestrsize() + 4 + 1/*subtype*/;
            break;
        case RegEx: {
            const char *p = value();
            size_t len1 = ( maxLen == -1 ) ? strlen( p ) : (size_t)mongo::strnlen( p, remain );
            //massert( 10318 ,  "Invalid regex string", len1 != -1 ); // ERH - 4/28/10 - don't think this does anything
            p = p + len1 + 1;
            size_t len2;
            if( maxLen == -1 )
                len2 = strlen( p );
            else {
                size_t x = remain - len1 - 1;
                verify( x <= 0x7fffffff );
                len2 = mongo::strnlen( p, (int) x );
            }
            //massert( 10319 ,  "Invalid regex options string", len2 != -1 ); // ERH - 4/28/10 - don't think this does anything
            x = (int) (len1 + 1 + len2 + 1);
        }
        break;
        default: {
            StringBuilder ss;
            ss << "BSONElement: bad type " << (int) type();
            std::string msg = ss.str();
            massert( 13655 , msg.c_str(),false);
        }
        }
        totalSize =  x + fieldNameSize() + 1; // BSONType

        return totalSize;
    }

    int BSONElement::size() const {
        if ( totalSize >= 0 )
            return totalSize;

        int x = 0;
        switch ( type() ) {
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            break;
        case mongo::Bool:
            x = 1;
            break;
        case NumberInt:
            x = 4;
            break;
        case Timestamp:
        case mongo::Date:
        case NumberDouble:
        case NumberLong:
            x = 8;
            break;
        case jstOID:
            x = 12;
            break;
        case Symbol:
        case Code:
        case mongo::String:
            x = valuestrsize() + 4;
            break;
        case DBRef:
            x = valuestrsize() + 4 + 12;
            break;
        case CodeWScope:
        case Object:
        case mongo::Array:
            x = objsize();
            break;
        case BinData:
            x = valuestrsize() + 4 + 1/*subtype*/;
            break;
        case RegEx: 
            {
                const char *p = value();
                size_t len1 = strlen(p);
                p = p + len1 + 1;
                size_t len2;
                len2 = strlen( p );
                x = (int) (len1 + 1 + len2 + 1);
            }
            break;
        default: 
            {
                StringBuilder ss;
                ss << "BSONElement: bad type " << (int) type();
                std::string msg = ss.str();
                massert(10320 , msg.c_str(),false);
            }
        }
        totalSize =  x + fieldNameSize() + 1; // BSONType

        return totalSize;
    }

    std::string BSONElement::toString( bool includeFieldName, bool full ) const {
        StringBuilder s;
        toString(s, includeFieldName, full);
        return s.str();
    }
    
    void BSONElement::toString( StringBuilder& s, bool includeFieldName, bool full, int depth ) const {

        if ( depth > BSONObj::maxToStringRecursionDepth ) {
            // check if we want the full/complete string
            if ( full ) {
                StringBuilder s;
                s << "Reached maximum recursion depth of ";
                s << BSONObj::maxToStringRecursionDepth;
                uassert(16150, s.str(), full != true);
            }
            s << "...";
            return;
        }

        if ( includeFieldName && type() != EOO )
            s << fieldName() << ": ";
        switch ( type() ) {
        case EOO:
            s << "EOO";
            break;
        case mongo::Date:
            s << "new Date(" << (long long) date() << ')';
            break;
        case RegEx: {
            s << "/" << regex() << '/';
            const char *p = regexFlags();
            if ( p ) s << p;
        }
        break;
        case NumberDouble:
            s.appendDoubleNice( number() );
            break;
        case NumberLong:
            s << _numberLong();
            break;
        case NumberInt:
            s << _numberInt();
            break;
        case mongo::Bool:
            s << ( boolean() ? "true" : "false" );
            break;
        case Object:
            embeddedObject().toString(s, false, full, depth+1);
            break;
        case mongo::Array:
            embeddedObject().toString(s, true, full, depth+1);
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
              << codeWScopeCode() << ", " << codeWScopeObject().toString(false, full) << ")";
            break;
        case Code:
            if ( !full &&  valuestrsize() > 80 ) {
                s.write(valuestr(), 70);
                s << "...";
            }
            else {
                s.write(valuestr(), valuestrsize()-1);
            }
            break;
        case Symbol:
        case mongo::String:
            s << '"';
            if ( !full &&  valuestrsize() > 160 ) {
                s.write(valuestr(), 150);
                s << "...\"";
            }
            else {
                s.write(valuestr(), valuestrsize()-1);
                s << '"';
            }
            break;
        case DBRef:
            s << "DBRef('" << valuestr() << "',";
            {
                mongo::OID x;
                std::memcpy(&x, valuestr() + valuestrsize(), sizeof(x));
                s << x << ')';
            }
            break;
        case jstOID:
            s << "ObjectId('";
            s << __oid() << "')";
            break;
        case BinData:
            s << "BinData(" << binDataType() << ", ";
            {
                int len;
                const char *data = binDataClean(len);
                if ( !full && len > 80 ) {
                    s << toHex(data, 70) << "...)";
                }
                else {
                    s << toHex(data, len) << ")";
                }
            }
            break;
        case Timestamp:
            s << "Timestamp " << timestampTime() << "|" << timestampInc();
            break;
        default:
            s << "?type=" << type();
            break;
        }
    }

    std::string BSONElement::_asCode() const {
        switch( type() ) {
        case mongo::String:
        case Code:
            return std::string(valuestr(), valuestrsize()-1);
        case CodeWScope:
            return std::string(codeWScopeCode(), *(int*)(valuestr())-1);
        default:
            log() << "can't convert type: " << (int)(type()) << " to code" << std::endl;
        }
        uassert( 10062 ,  "not code" , 0 );
        return "";
    }

    std::ostream& operator<<( std::ostream &s, const BSONElement &e ) {
        return s << e.toString();
    }

    StringBuilder& operator<<( StringBuilder &s, const BSONElement &e ) {
        e.toString( s );
        return s;
    }

    template<> bool BSONElement::coerce<std::string>( std::string* out ) const {
        if ( type() != mongo::String )
            return false;
        *out = String();
        return true;
    }

    template<> bool BSONElement::coerce<int>( int* out ) const {
        if ( !isNumber() )
            return false;
        *out = numberInt();
        return true;
    }

    template<> bool BSONElement::coerce<double>( double* out ) const {
        if ( !isNumber() )
            return false;
        *out = numberDouble();
        return true;
    }

    template<> bool BSONElement::coerce<bool>( bool* out ) const {
        *out = trueValue();
        return true;
    }

    template<> bool BSONElement::coerce< std::vector<std::string> >( std::vector<std::string>* out ) const {
        if ( type() != mongo::Array )
            return false;
        return Obj().coerceVector<std::string>( out );
    }

    template<typename T> bool BSONObj::coerceVector( std::vector<T>* out ) const {
        BSONObjIterator i( *this );
        while ( i.more() ) {
            BSONElement e = i.next();
            T t;
            if ( ! e.coerce<T>( &t ) )
                return false;
            out->push_back( t );
        }
        return true;
    }

    // used by jsonString()
    std::string escape( const std::string& s , bool escape_slash) {
        StringBuilder ret;
        for ( std::string::const_iterator i = s.begin(); i != s.end(); ++i ) {
            switch ( *i ) {
            case '"':
                ret << "\\\"";
                break;
            case '\\':
                ret << "\\\\";
                break;
            case '/':
                ret << (escape_slash ? "\\/" : "/");
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
                    //TODO: these should be utf16 code-units not bytes
                    char c = *i;
                    ret << "\\u00" << toHexLower(&c, 1);
                }
                else {
                    ret << *i;
                }
            }
        }
        return ret.str();
    }

    /* must be same type when called, unless both sides are #s 
       this large function is in header to facilitate inline-only use of bson
    */
    int compareElementValues(const BSONElement& l, const BSONElement& r) {
        int f;

        switch ( l.type() ) {
        case EOO:
        case Undefined: // EOO and Undefined are same canonicalType
        case jstNULL:
        case MaxKey:
        case MinKey:
            f = l.canonicalType() - r.canonicalType();
            if ( f<0 ) return -1;
            return f==0 ? 0 : 1;
        case Bool:
            return *l.value() - *r.value();
        case Timestamp:
            // unsigned compare for timestamps - note they are not really dates but (ordinal + time_t)
            if ( l.date() < r.date() )
                return -1;
            return l.date() == r.date() ? 0 : 1;
        case Date:
            {
                long long a = (long long) l.Date().millis;
                long long b = (long long) r.Date().millis;
                if( a < b ) 
                    return -1;
                return a == b ? 0 : 1;
            }
        case NumberLong:
            if( r.type() == NumberLong ) {
                long long L = l._numberLong();
                long long R = r._numberLong();
                if( L < R ) return -1;
                if( L == R ) return 0;
                return 1;
            }
            goto dodouble;
        case NumberInt:
            if( r.type() == NumberInt ) {
                int L = l._numberInt();
                int R = r._numberInt();
                if( L < R ) return -1;
                return L == R ? 0 : 1;
            }
            // else fall through
        case NumberDouble: 
dodouble:
            {
                double left = l.number();
                double right = r.number();
                if( left < right ) 
                    return -1;
                if( left == right )
                    return 0;
                if( isNaN(left) )
                    return isNaN(right) ? 0 : -1;
                return 1;
            }
        case jstOID:
            return memcmp(l.value(), r.value(), 12);
        case Code:
        case Symbol:
        case String:
            /* todo: a utf sort order version one day... */
            {
                // we use memcmp as we allow zeros in UTF8 strings
                int lsz = l.valuestrsize();
                int rsz = r.valuestrsize();
                int common = std::min(lsz, rsz);
                int res = memcmp(l.valuestr(), r.valuestr(), common);
                if( res ) 
                    return res;
                // longer std::string is the greater one
                return lsz-rsz;
            }
        case Object:
        case Array:
            return l.embeddedObject().woCompare( r.embeddedObject() );
        case DBRef: {
            int lsz = l.valuesize();
            int rsz = r.valuesize();
            if ( lsz - rsz != 0 ) return lsz - rsz;
            return memcmp(l.value(), r.value(), lsz);
        }
        case BinData: {
            int lsz = l.objsize(); // our bin data size in bytes, not including the subtype byte
            int rsz = r.objsize();
            if ( lsz - rsz != 0 ) return lsz - rsz;
            return memcmp(l.value()+4, r.value()+4, lsz+1 /*+1 for subtype byte*/);
        }
        case RegEx: {
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
            f = strcmp( l.codeWScopeScopeDataUnsafe() , r.codeWScopeScopeDataUnsafe() );
            if ( f )
                return f;
            return 0;
        }
        default:
            verify( false);
        }
        return -1;
    }
 

} // namespace mongo
