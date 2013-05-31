/** @file bsoninlines.h
          a goal here is that the most common bson methods can be used inline-only, a la boost.
          thus some things are inline that wouldn't necessarily be otherwise.
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

#pragma once

#include <map>
#include <limits>


#if defined(_WIN32)
#undef max
#undef min
#endif

namespace mongo {

    /* must be same type when called, unless both sides are #s 
       this large function is in header to facilitate inline-only use of bson
    */
    inline int compareElementValues(const BSONElement& l, const BSONElement& r) {
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
                // longer string is the greater one
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

    /* wo = "well ordered" 
       note: (mongodb related) : this can only change in behavior when index version # changes
    */
    inline int BSONElement::woCompare( const BSONElement &e,
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

    inline BSONObjIterator BSONObj::begin() const {
        return BSONObjIterator(*this);
    }

    inline BSONObj BSONElement::embeddedObjectUserCheck() const {
        if ( MONGO_likely(isABSONObj()) )
            return BSONObj(value());
        std::stringstream ss;
        ss << "invalid parameter: expected an object (" << fieldName() << ")";
        uasserted( 10065 , ss.str() );
        return BSONObj(); // never reachable
    }

    inline BSONObj BSONElement::embeddedObject() const {
        verify( isABSONObj() );
        return BSONObj(value());
    }

    inline BSONObj BSONElement::codeWScopeObject() const {
        verify( type() == CodeWScope );
        int strSizeWNull = *(int *)( value() + 4 );
        return BSONObj( value() + 4 + 4 + strSizeWNull );
    }

    // deep (full) equality
    inline bool BSONObj::equal(const BSONObj &rhs) const {
        BSONObjIterator i(*this);
        BSONObjIterator j(rhs);
        BSONElement l,r;
        do {
            // so far, equal...
            l = i.next();
            r = j.next();
            if ( l.eoo() )
                return r.eoo();
        } while( l == r );
        return false;
    }

    inline NOINLINE_DECL void BSONObj::_assertInvalid() const {
        StringBuilder ss;
        int os = objsize();
        ss << "BSONObj size: " << os << " (0x" << integerToHex( os ) << ") is invalid. "
           << "Size must be between 0 and " << BSONObjMaxInternalSize
           << "(" << ( BSONObjMaxInternalSize/(1024*1024) ) << "MB)";
        try {
            BSONElement e = firstElement();
            ss << " First element: " << e.toString();
        }
        catch ( ... ) { }
        massert( 10334 , ss.str() , 0 );
    }

    /* the idea with NOINLINE_DECL here is to keep this from inlining in the
       getOwned() method.  the presumption being that is better.
    */
    inline NOINLINE_DECL BSONObj BSONObj::copy() const {
        Holder *h = (Holder*) malloc(objsize() + sizeof(unsigned));
        h->zero();
        memcpy(h->data, objdata(), objsize());
        return BSONObj(h);
    }

    inline BSONObj BSONObj::getOwned() const {
        if ( isOwned() )
            return *this;
        return copy();
    }

    // wrap this element up as a singleton object.
    inline BSONObj BSONElement::wrap() const {
        BSONObjBuilder b(size()+6);
        b.append(*this);
        return b.obj();
    }

    inline BSONObj BSONElement::wrap( const StringData& newName ) const {
        BSONObjBuilder b(size() + 6 + newName.size());
        b.appendAs(*this,newName);
        return b.obj();
    }

    inline void BSONObj::getFields(unsigned n, const char **fieldNames, BSONElement *fields) const { 
        BSONObjIterator i(*this);
        while ( i.more() ) {
            BSONElement e = i.next();
            const char *p = e.fieldName();
            for( unsigned i = 0; i < n; i++ ) {
                if( strcmp(p, fieldNames[i]) == 0 ) {
                    fields[i] = e;
                    break;
                }
            }
        }
    }

    inline BSONElement BSONObj::getField(const StringData& name) const {
        BSONObjIterator i(*this);
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( name == e.fieldName() )
                return e;
        }
        return BSONElement();
    }

    inline int BSONObj::getIntField(const StringData& name) const {
        BSONElement e = getField(name);
        return e.isNumber() ? (int) e.number() : std::numeric_limits< int >::min();
    }

    inline bool BSONObj::getBoolField(const StringData& name) const {
        BSONElement e = getField(name);
        return e.type() == Bool ? e.boolean() : false;
    }

    inline const char * BSONObj::getStringField(const StringData& name) const {
        BSONElement e = getField(name);
        return e.type() == String ? e.valuestr() : "";
    }

    /* add all the fields from the object specified to this object */
    inline BSONObjBuilder& BSONObjBuilder::appendElements(BSONObj x) {
        if (!x.isEmpty())
            _b.appendBuf(
                x.objdata() + 4,   // skip over leading length
                x.objsize() - 5);  // ignore leading length and trailing \0
        return *this;
    }

    /* add all the fields from the object specified to this object if they don't exist */
    inline BSONObjBuilder& BSONObjBuilder::appendElementsUnique(BSONObj x) {
        std::set<std::string> have;
        {
            BSONObjIterator i = iterator();
            while ( i.more() )
                have.insert( i.next().fieldName() );
        }
        
        BSONObjIterator it(x);
        while ( it.more() ) {
            BSONElement e = it.next();
            if ( have.count( e.fieldName() ) )
                continue;
            append(e);
        }
        return *this;
    }


    inline bool BSONObj::isValid() const {
        int x = objsize();
        return x > 0 && x <= BSONObjMaxInternalSize;
    }

    inline bool BSONObj::getObjectID(BSONElement& e) const {
        BSONElement f = getField("_id");
        if( !f.eoo() ) {
            e = f;
            return true;
        }
        return false;
    }

    inline BSONObjBuilderValueStream::BSONObjBuilderValueStream( BSONObjBuilder * builder ) {
        _builder = builder;
    }

    template<class T>
    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<( T value ) {
        _builder->append(_fieldName, value);
        _fieldName = StringData();
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<( const BSONElement& e ) {
        _builder->appendAs( e , _fieldName );
        _fieldName = StringData();
        return *_builder;
    }

    inline BufBuilder& BSONObjBuilderValueStream::subobjStart() {
        StringData tmp = _fieldName;
        _fieldName = StringData();
        return _builder->subobjStart(tmp);
    }

    inline BufBuilder& BSONObjBuilderValueStream::subarrayStart() {
        StringData tmp = _fieldName;
        _fieldName = StringData();
        return _builder->subarrayStart(tmp);
    }

    inline Labeler BSONObjBuilderValueStream::operator<<( const Labeler::Label &l ) {
        return Labeler( l, this );
    }

    inline void BSONObjBuilderValueStream::endField( const StringData& nextFieldName ) {
        if ( haveSubobj() ) {
            verify( _fieldName.rawData() );
            _builder->append( _fieldName, subobj()->done() );
        }
        _subobj.reset();
        _fieldName = nextFieldName;
    }

    inline BSONObjBuilder *BSONObjBuilderValueStream::subobj() {
        if ( !haveSubobj() )
            _subobj.reset( new BSONObjBuilder() );
        return _subobj.get();
    }

    template<class T> inline
    BSONObjBuilder& Labeler::operator<<( T value ) {
        s_->subobj()->append( l_.l_, value );
        return *s_->_builder;
    }

    inline
    BSONObjBuilder& Labeler::operator<<( const BSONElement& e ) {
        s_->subobj()->appendAs( e, l_.l_ );
        return *s_->_builder;
    }

    // {a: {b:1}} -> {a.b:1}
    void nested2dotted(BSONObjBuilder& b, const BSONObj& obj, const std::string& base="");
    inline BSONObj nested2dotted(const BSONObj& obj) {
        BSONObjBuilder b;
        nested2dotted(b, obj);
        return b.obj();
    }

    // {a.b:1} -> {a: {b:1}}
    void dotted2nested(BSONObjBuilder& b, const BSONObj& obj);
    inline BSONObj dotted2nested(const BSONObj& obj) {
        BSONObjBuilder b;
        dotted2nested(b, obj);
        return b.obj();
    }

    inline BSONObjIterator BSONObjBuilder::iterator() const {
        const char * s = _b.buf() + _offset;
        const char * e = _b.buf() + _b.len();
        return BSONObjIterator( s , e );
    }

    inline bool BSONObjBuilder::hasField( const StringData& name ) const {
        BSONObjIterator i = iterator();
        while ( i.more() )
            if ( name == i.next().fieldName() )
                return true;
        return false;
    }

    /* WARNING: nested/dotted conversions are not 100% reversible
     * nested2dotted(dotted2nested({a.b: {c:1}})) -> {a.b.c: 1}
     * also, dotted2nested ignores order
     */

    typedef std::map<std::string, BSONElement> BSONMap;
    inline BSONMap bson2map(const BSONObj& obj) {
        BSONMap m;
        BSONObjIterator it(obj);
        while (it.more()) {
            BSONElement e = it.next();
            m[e.fieldName()] = e;
        }
        return m;
    }

    struct BSONElementFieldNameCmp {
        bool operator()( const BSONElement &l, const BSONElement &r ) const {
            return strcmp( l.fieldName() , r.fieldName() ) <= 0;
        }
    };

    typedef std::set<BSONElement, BSONElementFieldNameCmp> BSONSortedElements;
    inline BSONSortedElements bson2set( const BSONObj& obj ) {
        BSONSortedElements s;
        BSONObjIterator it(obj);
        while ( it.more() )
            s.insert( it.next() );
        return s;
    }

    inline std::string BSONObj::toString( bool isArray, bool full ) const {
        if ( isEmpty() ) return (isArray ? "[]" : "{}");
        StringBuilder s;
        toString(s, isArray, full);
        return s.str();
    }
    inline void BSONObj::toString( StringBuilder& s,  bool isArray, bool full, int depth ) const {
        if ( isEmpty() ) {
            s << (isArray ? "[]" : "{}");
            return;
        }

        s << ( isArray ? "[ " : "{ " );
        BSONObjIterator i(*this);
        bool first = true;
        while ( 1 ) {
            massert( 10327 ,  "Object does not end with EOO", i.moreWithEOO() );
            BSONElement e = i.next( true );
            massert( 10328 ,  "Invalid element size", e.size() > 0 );
            massert( 10329 ,  "Element too large", e.size() < ( 1 << 30 ) );
            int offset = (int) (e.rawdata() - this->objdata());
            massert( 10330 ,  "Element extends past end of object",
                     e.size() + offset <= this->objsize() );
            e.validate();
            bool end = ( e.size() + offset == this->objsize() );
            if ( e.eoo() ) {
                massert( 10331 ,  "EOO Before end of object", end );
                break;
            }
            if ( first )
                first = false;
            else
                s << ", ";
            e.toString( s, !isArray, full, depth );
        }
        s << ( isArray ? " ]" : " }" );
    }

    inline void BSONElement::validate() const {
        const BSONType t = type();

        switch( t ) {
        case DBRef:
        case Code:
        case Symbol:
        case mongo::String: {
            unsigned x = (unsigned) valuestrsize();
            bool lenOk = x > 0 && x < (unsigned) BSONObjMaxInternalSize;
            if( lenOk && valuestr()[x-1] == 0 )
                return;
            StringBuilder buf;
            buf <<  "Invalid dbref/code/string/symbol size: " << x;
            if( lenOk )
                buf << " strnlen:" << mongo::strnlen( valuestr() , x );
            msgasserted( 10321 , buf.str() );
            break;
        }
        case CodeWScope: {
            int totalSize = *( int * )( value() );
            massert( 10322 ,  "Invalid CodeWScope size", totalSize >= 8 );
            int strSizeWNull = *( int * )( value() + 4 );
            massert( 10323 ,  "Invalid CodeWScope string size", totalSize >= strSizeWNull + 4 + 4 );
            massert( 10324 ,  "Invalid CodeWScope string size",
                     strSizeWNull > 0 &&
                     (strSizeWNull - 1) == mongo::strnlen( codeWScopeCode(), strSizeWNull ) );
            massert( 10325 ,  "Invalid CodeWScope size", totalSize >= strSizeWNull + 4 + 4 + 4 );
            int objSize = *( int * )( value() + 4 + 4 + strSizeWNull );
            massert( 10326 ,  "Invalid CodeWScope object size", totalSize == 4 + 4 + strSizeWNull + objSize );
            // Subobject validation handled elsewhere.
        }
        case Object:
            // We expect Object size validation to be handled elsewhere.
        default:
            break;
        }
    }

    inline int BSONElement::size( int maxLen ) const {
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

    inline int BSONElement::size() const {
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

    inline std::string BSONElement::toString( bool includeFieldName, bool full ) const {
        StringBuilder s;
        toString(s, includeFieldName, full);
        return s.str();
    }
    inline void BSONElement::toString( StringBuilder& s, bool includeFieldName, bool full, int depth ) const {

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
                mongo::OID *x = (mongo::OID *) (valuestr() + valuestrsize());
                s << *x << ')';
            }
            break;
        case jstOID:
            s << "ObjectId('";
            s << __oid() << "')";
            break;
        case BinData:
            s << "BinData";
            if (full) {
                int len;
                const char* data = binDataClean(len);
                s << '(' << binDataType() << ", " << toHex(data, len) << ')';
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

    /* return has eoo() true if no match
       supports "." notation to reach into embedded objects
    */
    inline BSONElement BSONObj::getFieldDotted(const StringData& name) const {
        BSONElement e = getField(name);
        if (e.eoo()) {
            size_t dot_offset = name.find('.');
            if (dot_offset != string::npos) {
                StringData left = name.substr(0, dot_offset);
                StringData right = name.substr(dot_offset + 1);
                BSONObj sub = getObjectField(left);
                return sub.isEmpty() ? BSONElement() : sub.getFieldDotted(right);
            }
        }

        return e;
    }

    inline BSONObj BSONObj::getObjectField(const StringData& name) const {
        BSONElement e = getField(name);
        BSONType t = e.type();
        return t == Object || t == Array ? e.embeddedObject() : BSONObj();
    }

    inline int BSONObj::nFields() const {
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

    inline BSONObj::BSONObj() {
        /* little endian ordering here, but perhaps that is ok regardless as BSON is spec'd
           to be little endian external to the system. (i.e. the rest of the implementation of bson,
           not this part, fails to support big endian)
        */
        static char p[] = { /*size*/5, 0, 0, 0, /*eoo*/0 };
        _objdata = p;
    }

    inline BSONObj BSONElement::Obj() const { return embeddedObjectUserCheck(); }

    inline BSONElement BSONElement::operator[] (const std::string& field) const {
        BSONObj o = Obj();
        return o[field];
    }

    inline void BSONObj::elems(std::vector<BSONElement> &v) const {
        BSONObjIterator i(*this);
        while( i.more() )
            v.push_back(i.next());
    }

    inline void BSONObj::elems(std::list<BSONElement> &v) const {
        BSONObjIterator i(*this);
        while( i.more() )
            v.push_back(i.next());
    }

    template <class T>
    void BSONObj::Vals(std::vector<T>& v) const {
        BSONObjIterator i(*this);
        while( i.more() ) {
            T t;
            i.next().Val(t);
            v.push_back(t);
        }
    }
    template <class T>
    void BSONObj::Vals(std::list<T>& v) const {
        BSONObjIterator i(*this);
        while( i.more() ) {
            T t;
            i.next().Val(t);
            v.push_back(t);
        }
    }

    template <class T>
    void BSONObj::vals(std::vector<T>& v) const {
        BSONObjIterator i(*this);
        while( i.more() ) {
            try {
                T t;
                i.next().Val(t);
                v.push_back(t);
            }
            catch(...) { }
        }
    }
    template <class T>
    void BSONObj::vals(std::list<T>& v) const {
        BSONObjIterator i(*this);
        while( i.more() ) {
            try {
                T t;
                i.next().Val(t);
                v.push_back(t);
            }
            catch(...) { }
        }
    }

    inline std::ostream& operator<<( std::ostream &s, const BSONObj &o ) {
        return s << o.toString();
    }

    inline std::ostream& operator<<( std::ostream &s, const BSONElement &e ) {
        return s << e.toString();
    }

    inline StringBuilder& operator<<( StringBuilder &s, const BSONObj &o ) {
        o.toString( s );
        return s;
    }
    inline StringBuilder& operator<<( StringBuilder &s, const BSONElement &e ) {
        e.toString( s );
        return s;
    }


    inline void BSONElement::Val(BSONObj& v) const { v = Obj(); }

    template<typename T>
    inline BSONFieldValue<BSONObj> BSONField<T>::query( const char * q , const T& t ) const {
        BSONObjBuilder b;
        b.append( q , t );
        return BSONFieldValue<BSONObj>( _name , b.obj() );
    }

    // used by jsonString()
    inline std::string escape( const std::string& s , bool escape_slash=false) {
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

    inline std::string BSONObj::hexDump() const {
        std::stringstream ss;
        const char *d = objdata();
        int size = objsize();
        for( int i = 0; i < size; ++i ) {
            ss.width( 2 );
            ss.fill( '0' );
            ss << std::hex << (unsigned)(unsigned char)( d[ i ] ) << std::dec;
            if ( ( d[ i ] >= '0' && d[ i ] <= '9' ) || ( d[ i ] >= 'A' && d[ i ] <= 'z' ) )
                ss << '\'' << d[ i ] << '\'';
            if ( i != size - 1 )
                ss << ' ';
        }
        return ss.str();
    }

    inline void BSONObjBuilder::appendKeys( const BSONObj& keyPattern , const BSONObj& values ) {
        BSONObjIterator i(keyPattern);
        BSONObjIterator j(values);

        while ( i.more() && j.more() ) {
            appendAs( j.next() , i.next().fieldName() );
        }

        verify( ! i.more() );
        verify( ! j.more() );
    }

    inline BSONObj BSONObj::removeField(const StringData& name) const {
        BSONObjBuilder b;
        BSONObjIterator i(*this);
        while ( i.more() ) {
            BSONElement e = i.next();
            const char *fname = e.fieldName();
            if ( name != fname )
                b.append(e);
        }
        return b.obj();
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


    template<> inline bool BSONElement::coerce<std::string>( std::string* out ) const {
        if ( type() != mongo::String )
            return false;
        *out = String();
        return true;
    }

    template<> inline bool BSONElement::coerce<int>( int* out ) const {
        if ( !isNumber() )
            return false;
        *out = numberInt();
        return true;
    }

    template<> inline bool BSONElement::coerce<double>( double* out ) const {
        if ( !isNumber() )
            return false;
        *out = numberDouble();
        return true;
    }

    template<> inline bool BSONElement::coerce<bool>( bool* out ) const {
        *out = trueValue();
        return true;
    }

    template<> inline bool BSONElement::coerce< std::vector<std::string> >( std::vector<std::string>* out ) const {
        if ( type() != mongo::Array )
            return false;
        return Obj().coerceVector<std::string>( out );
    }


}
