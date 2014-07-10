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

} // namespace mongo
