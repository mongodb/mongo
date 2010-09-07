// bsoninlines.h

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
#include "util/atomic_int.h"
#include "util/misc.h"
#include "../util/hex.h"

namespace mongo {

    inline BSONObjIterator BSONObj::begin() { 
        return BSONObjIterator(*this);
    }

    inline BSONObj BSONElement::embeddedObjectUserCheck() const {
        uassert( 10065 ,  "invalid parameter: expected an object", isABSONObj() );
        return BSONObj(value());
    }

    inline BSONObj BSONElement::embeddedObject() const {
        assert( isABSONObj() );
        return BSONObj(value());
    }

    inline BSONObj BSONElement::codeWScopeObject() const {
        assert( type() == CodeWScope );
        int strSizeWNull = *(int *)( value() + 4 );
        return BSONObj( value() + 4 + 4 + strSizeWNull );
    }
    
    inline BSONObj BSONObj::copy() const {
        char *p = (char*) malloc(objsize());
        memcpy(p, objdata(), objsize());
        return BSONObj(p, true);
    }

    // wrap this element up as a singleton object.
    inline BSONObj BSONElement::wrap() const {
        BSONObjBuilder b(size()+6);
        b.append(*this);
        return b.obj();
    }

    inline BSONObj BSONElement::wrap( const char * newName ) const {
        BSONObjBuilder b(size()+6+(int)strlen(newName));
        b.appendAs(*this,newName);
        return b.obj();
    }


    inline bool BSONObj::hasElement(const char *name) const {
        if ( !isEmpty() ) {
            BSONObjIterator it(*this);
            while ( it.moreWithEOO() ) {
                BSONElement e = it.next();
                if ( strcmp(name, e.fieldName()) == 0 )
                    return true;
            }
        }
        return false;
    }

    inline BSONElement BSONObj::getField(const StringData& name) const {
        BSONObjIterator i(*this);
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp(e.fieldName(), name.data()) == 0 )
                return e;
        }
        return BSONElement();
    }

    /* add all the fields from the object specified to this object */
    inline BSONObjBuilder& BSONObjBuilder::appendElements(BSONObj x) {
        BSONObjIterator it(x);
        while ( it.moreWithEOO() ) {
            BSONElement e = it.next();
            if ( e.eoo() ) break;
            append(e);
        }
        return *this;
    }

    inline bool BSONObj::isValid(){
        int x = objsize();
        return x > 0 && x <= 1024 * 1024 * 8;
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
        _fieldName = 0;
        _builder = builder;
    }
    
    template<class T> 
    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<( T value ) { 
        _builder->append(_fieldName, value);
        _fieldName = 0;
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<( const BSONElement& e ) { 
        _builder->appendAs( e , _fieldName );
        _fieldName = 0;
        return *_builder;
    }

    inline Labeler BSONObjBuilderValueStream::operator<<( const Labeler::Label &l ) { 
        return Labeler( l, this );
    }

    inline void BSONObjBuilderValueStream::endField( const char *nextFieldName ) {
        if ( _fieldName && haveSubobj() ) {
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
    void nested2dotted(BSONObjBuilder& b, const BSONObj& obj, const string& base="");
    inline BSONObj nested2dotted(const BSONObj& obj){
        BSONObjBuilder b;
        nested2dotted(b, obj);
        return b.obj();
    }

    // {a.b:1} -> {a: {b:1}}
    void dotted2nested(BSONObjBuilder& b, const BSONObj& obj);
    inline BSONObj dotted2nested(const BSONObj& obj){
        BSONObjBuilder b;
        dotted2nested(b, obj);
        return b.obj();
    }

    inline BSONObjIterator BSONObjBuilder::iterator() const {
        const char * s = _b.buf() + _offset;
        const char * e = _b.buf() + _b.len();
        return BSONObjIterator( s , e );
    }
    
    /* WARNING: nested/dotted conversions are not 100% reversible
     * nested2dotted(dotted2nested({a.b: {c:1}})) -> {a.b.c: 1}
     * also, dotted2nested ignores order
     */

    typedef map<string, BSONElement> BSONMap;
    inline BSONMap bson2map(const BSONObj& obj){
        BSONMap m;
        BSONObjIterator it(obj);
        while (it.more()){
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

    typedef set<BSONElement, BSONElementFieldNameCmp> BSONSortedElements;
    inline BSONSortedElements bson2set( const BSONObj& obj ){
        BSONSortedElements s;
        BSONObjIterator it(obj);
        while ( it.more() )
            s.insert( it.next() );
        return s;
    }

    inline string BSONObj::toString( bool isArray, bool full ) const {
        if ( isEmpty() ) return "{}";
        StringBuilder s;
        toString(s, isArray, full);
        return s.str();
    }
    inline void BSONObj::toString(StringBuilder& s,  bool isArray, bool full ) const {
        if ( isEmpty() ){
            s << "{}";
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
            e.toString(s, !isArray, full );
        }
        s << ( isArray ? " ]" : " }" );
    }

    extern unsigned getRandomNumber();

    inline void BSONElement::validate() const {
        const BSONType t = type();
        
        switch( t ) {
        case DBRef:
        case Code:
        case Symbol:
        case mongo::String: {
            int x = valuestrsize();
            if ( x > 0 && x < BSONObjMaxSize && valuestr()[x-1] == 0 )
                return;
            StringBuilder buf;
            buf <<  "Invalid dbref/code/string/symbol size: " << x << " strnlen:" << mongo::strnlen( valuestr() , x );
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
        case RegEx:
        {
            const char *p = value();
            size_t len1 = ( maxLen == -1 ) ? strlen( p ) : mongo::strnlen( p, remain );
            //massert( 10318 ,  "Invalid regex string", len1 != -1 ); // ERH - 4/28/10 - don't think this does anything
            p = p + len1 + 1;
            size_t len2;
            if( maxLen == -1 )
                len2 = strlen( p );
            else {
                size_t x = remain - len1 - 1;
                assert( x <= 0x7fffffff );
                len2 = mongo::strnlen( p, (int) x );
            }
            //massert( 10319 ,  "Invalid regex options string", len2 != -1 ); // ERH - 4/28/10 - don't think this does anything
            x = (int) (len1 + 1 + len2 + 1);
        }
        break;
        default: {
            StringBuilder ss;
            ss << "BSONElement: bad type " << (int) type();
            string msg = ss.str();
            massert( 10320 , msg.c_str(),false);
        }
        }
        totalSize =  x + fieldNameSize() + 1; // BSONType

        return totalSize;
    }

    inline string BSONElement::toString( bool includeFieldName, bool full ) const {
        StringBuilder s;
        toString(s, includeFieldName, full);
        return s.str();
    }
    inline void BSONElement::toString(StringBuilder& s, bool includeFieldName, bool full ) const {
        if ( includeFieldName && type() != EOO )
            s << fieldName() << ": ";
        switch ( type() ) {
        case EOO:
            s << "EOO";
            break;
        case mongo::Date:
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
            embeddedObject().toString(s, false, full);
            break;
        case mongo::Array:
            embeddedObject().toString(s, true, full);
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
            } else {
                s.write(valuestr(), valuestrsize()-1);
            }
            break;
        case Symbol:
        case mongo::String:
            s << '"';
            if ( !full &&  valuestrsize() > 80 ) {
                s.write(valuestr(), 70);
                s << "...\"";
            } else {
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
            if (full){
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
    inline BSONElement BSONObj::getFieldDotted(const char *name) const {
        BSONElement e = getField( name );
        if ( e.eoo() ) {
            const char *p = strchr(name, '.');
            if ( p ) {
                string left(name, p-name);
                BSONObj sub = getObjectField(left.c_str());
                return sub.isEmpty() ? BSONElement() : sub.getFieldDotted(p+1);
            }
        }

        return e;
    }

    inline BSONObj BSONObj::getObjectField(const char *name) const {
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

    inline BSONElement BSONElement::operator[] (const string& field) const { 
        BSONObj o = Obj();
        return o[field];
    }

    inline void BSONObj::elems(vector<BSONElement> &v) const {
        BSONObjIterator i(*this);
        while( i.more() )
            v.push_back(i.next());
    }

    inline void BSONObj::elems(list<BSONElement> &v) const { 
        BSONObjIterator i(*this);
        while( i.more() )
            v.push_back(i.next());
    }

    template <class T>
    void BSONObj::Vals(vector<T>& v) const { 
        BSONObjIterator i(*this);
        while( i.more() ) {
            T t;
            i.next().Val(t);
            v.push_back(t);
        }
    }
    template <class T>
    void BSONObj::Vals(list<T>& v) const { 
        BSONObjIterator i(*this);
        while( i.more() ) {
            T t;
            i.next().Val(t);
            v.push_back(t);
        }
    }

    template <class T>
    void BSONObj::vals(vector<T>& v) const { 
        BSONObjIterator i(*this);
        while( i.more() ) {
            try {
                T t;
                i.next().Val(t);
                v.push_back(t);
            } catch(...) { }
        }
    }
    template <class T>
    void BSONObj::vals(list<T>& v) const { 
        BSONObjIterator i(*this);
        while( i.more() ) {
            try {
                T t;
                i.next().Val(t);
                v.push_back(t);
            } catch(...) { }
        }
    }

    inline ostream& operator<<( ostream &s, const BSONObj &o ) {
        return s << o.toString();
    }

    inline ostream& operator<<( ostream &s, const BSONElement &e ) {
        return s << e.toString();
    }

    inline void BSONElement::Val(BSONObj& v) const { v = Obj(); }

    template<typename T>
    inline BSONFieldValue<BSONObj> BSONField<T>::query( const char * q , const T& t ) const {
        BSONObjBuilder b;
        b.append( q , t );
        return BSONFieldValue<BSONObj>( _name , b.obj() );
    }
}
