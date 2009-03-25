// jsobj.cpp

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
#include "nonce.h"
#include "../util/goodies.h"
#include <limits>
#include "../util/unittest.h"
#include "json.h"
#include "repl.h"

namespace mongo {

    BSONElement nullElement;

    ostream& operator<<( ostream &s, const OID &o ) {
        s << o.str();
        return s;
    }

    string BSONElement::toString() const {
        stringstream s;
        switch ( type() ) {
        case EOO:
            return "EOO";
        case Date:
            s << fieldName() << ": Date(" << hex << date() << ')';
            break;
        case RegEx:
        {
            s << fieldName() << ": /" << regex() << '/';
            const char *p = regexFlags();
            if ( p ) s << p;
        }
        break;
        case NumberDouble:
			{
				s << fieldName() << ": ";
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
        case NumberInt:
            s.precision( 16 );
            s << fieldName() << ": " << number();
            //s << "(" << ( type() == NumberInt ? "int" : "double" ) << ")";
            break;
        case Bool:
            s << fieldName() << ": " << ( boolean() ? "true" : "false" );
            break;
        case Object:
        case Array:
            s << fieldName() << ": " << embeddedObject().toString();
            break;
        case Undefined:
            s << fieldName() << ": undefined";
            break;
        case jstNULL:
            s << fieldName() << ": null";
            break;
        case MaxKey:
            s << fieldName() << ": MaxKey";
            break;
        case MinKey:
            s << fieldName() << ": MinKey";
            break;
        case CodeWScope:
            s << fieldName() << ": CodeWScope( "
                << codeWScopeCode() << ", " << codeWScopeObject().toString() << ")";
            break;
        case Code:
            s << fieldName() << ": ";
            if ( valuestrsize() > 80 )
                s << string(valuestr()).substr(0, 70) << "...";
            else {
                s << valuestr();
            }
            break;
        case Symbol:
        case String:
            s << fieldName() << ": ";
            if ( valuestrsize() > 80 )
                s << '"' << string(valuestr()).substr(0, 70) << "...\"";
            else {
                s << '"' << valuestr() << '"';
            }
            break;
        case DBRef:
            s << fieldName();
            s << " : DBRef('" << valuestr() << "',";
            {
                OID *x = (OID *) (valuestr() + valuestrsize());
                s << *x << ')';
            }
            break;
        case jstOID:
            s << fieldName() << " : ObjId(";
            s << __oid() << ')';
            break;
        case BinData:
            s << fieldName() << " : BinData";
            break;
        case Timestamp:
            s << fieldName() << " : Timestamp " << timestampTime() << "|" << timestampInc();
            break;
        default:
            s << fieldName() << ": ?type=" << type();
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

    typedef boost::archive::iterators::base64_from_binary
    < boost::archive::iterators::transform_width
    < string::const_iterator, 6, 8 >
    > base64_t;

    string BSONElement::jsonString( JsonStringFormat format, bool includeFieldNames ) const {
        stringstream s;
        if ( includeFieldNames )
            s << '"' << escape( fieldName() ) << "\" : ";
        switch ( type() ) {
        case String:
        case Symbol:
            s << '"' << escape( valuestr() ) << '"';
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
                s << "{ \"$ns\" : ";
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
            if ( format == TenGen )
                s << "ObjectId( ";
            s << '"' << __oid() << '"';
            if ( format == TenGen )
                s << " )";
            break;
        case BinData: {
            int len = *(int *)( value() );
            BinDataType type = BinDataType( *(char *)( (int *)( value() ) + 1 ) );
            s << "{ \"$binary\" : \"";
            char *start = ( char * )( value() ) + sizeof( int ) + 1;
            string temp(start, len);
            string base64 = string( base64_t( temp.begin() ), base64_t( temp.end() ) );
            s << base64;
            int padding = ( 4 - ( base64.length() % 4 ) ) % 4;
            for ( int i = 0; i < padding; ++i )
                s << '=';
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

    int BSONElement::getGtLtOp() const {
        const char *fn = fieldName();
        if ( fn[0] == '$' && fn[1] ) {
            if ( fn[2] == 't' ) {
                if ( fn[1] == 'g' ) {
                    if ( fn[3] == 0 ) return JSMatcher::GT;
                    else if ( fn[3] == 'e' && fn[4] == 0 ) return JSMatcher::GTE;
                }
                else if ( fn[1] == 'l' ) {
                    if ( fn[3] == 0 ) return JSMatcher::LT;
                    else if ( fn[3] == 'e' && fn[4] == 0 ) return JSMatcher::LTE;
                }
            }
            else if ( fn[2] == 'e' ) {
                if ( fn[1] == 'n' && fn[3] == 0 )
                    return JSMatcher::NE;
            }
            else if ( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0 )
                return JSMatcher::opIN;
            else if ( fn[1] == 's' && fn[2] == 'i' && fn[3] == 'z' && fn[4] == 'e' && fn[5] == 0 )
                return JSMatcher::opSIZE;
        }
        return JSMatcher::Equality;
    }

    int BSONElement::woCompare( const BSONElement &e,
                                bool considerFieldName ) const {
        int lt = (int) type();
        if ( lt == NumberInt ) lt = NumberDouble;
        int rt = (int) e.type();
        if ( rt == NumberInt ) rt = NumberDouble;

        int x = lt - rt;
        if ( x != 0 )
            return x;
        if ( considerFieldName ) {
            x = strcmp(fieldName(), e.fieldName());
            if ( x != 0 )
                return x;
        }
        x = compareElementValues(*this, e);
        return x;
    }

    /* must be same type! */
    int compareElementValues(const BSONElement& l, const BSONElement& r) {
        int f;
        double x;
        switch ( l.type() ) {
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            f = l.type() - r.type();
            if ( f<0 ) return -1;
            return f==0 ? 0 : 1;
        case Bool:
            return *l.value() - *r.value();
        case Timestamp:
        case Date:
            if ( l.date() < r.date() )
                return -1;
            return l.date() == r.date() ? 0 : 1;
        case NumberInt:
        case NumberDouble:
            x = l.number() - r.number();
            if ( x < 0 ) return -1;
            return x == 0 ? 0 : 1;
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
        default:
            out() << "compareElementValues: bad type " << (int) l.type() << endl;
            assert(false);
        }
        return -1;
    }

    const char *BSONElement::simpleRegex() const {
        if ( *regexFlags() )                                                                     
            return 0;                                                                              
        const char *i = regex();                                                                 
        if ( *i != '^' )                                                                           
            return 0;                                                                              
        ++i;                                                                                       
        // Empty string matches everything, won't limit our search.                                
        if ( !*i )                                                                                 
            return 0;                                                                              
        for( ; *i; ++i )                                                                           
            if (!( *i == ' ' || (*i>='0'&&*i<='9') || (*i>='@'&&*i<='Z') || (*i>='a'&&*i<='z') ))  
                return 0;                                                                          
        return regex() + 1;              
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
            return JSMatcher::Equality;

        BSONElement fe = e.embeddedObject().firstElement();
        return fe.getGtLtOp();
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
            massert( "Object does not end with EOO", i.more() );
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
        while ( i.more() ) {
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
        /*
        	BSONObjIterator i(*this);
        	while( i.more() ) {
        		BSONElement e = i.next();
        		if( e.eoo() )
        			break;
        		if( strcmp(e.fieldName(), name) == 0 )
        			return e;
        	}
        	return nullElement;
        */
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
        while ( i.more() ) {
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
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = getField(e.fieldName());
            if ( !x.eoo() )
                b.appendAs(x, "");
        }
        return b.obj();
    }

    BSONObj BSONObj::extractFields(const BSONObj& pattern) const {
        BSONObjBuilder b(32); // scanandorder.h can make a zillion of these, so we start the allocation very small
        BSONObjIterator i(pattern);
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = getFieldDotted(e.fieldName());
            if ( x.eoo() )
                return BSONObj();
            b.append(x);
        }
        return b.obj();
    }

    BSONObj BSONObj::filterFieldsUndotted( const BSONObj &filter, bool inFilter ) const {
        BSONObjBuilder b;
        BSONObjIterator i( *this );
        while( i.more() ) {
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
        while( i.more() ) {
            BSONElement f = i.next();
            if ( f.eoo() )
                return BSONElement();
            if ( strcmp( f.fieldName(), fieldName ) == 0 )
                break;
            ++j;
        }
        BSONObjIterator k( *this );
        while( k.more() ) {
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
        while ( i.more() ) {
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
        while ( i.more() ) {
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
        while ( i.more() ) {
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
        while( i.more() ) {
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
        BSONElement f = j.more() ? j.next() : BSONObj().firstElement();
        while( i.more() ) {
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

    void OID::init() {
        static unsigned machine = (unsigned) security.getNonce();
        static unsigned inc = (unsigned) security.getNonce();

        unsigned t = (unsigned) time(0);
        char *T = (char *) &t;
        data[0] = T[3];
        data[1] = T[2];
        data[2] = T[1];
        data[3] = T[0];

        (unsigned&) data[4] = machine;
        ++inc;
        T = (char *) &inc;
        data[8] = T[3];
        data[9] = T[2];
        data[10] = T[1];
        data[11] = T[0];
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

} // namespace mongo
