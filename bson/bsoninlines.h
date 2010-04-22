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

namespace mongo {

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
        BSONObjBuilder b(size()+6+strlen(newName));
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

    inline BSONElement BSONObj::getField(const char *name) const {
        BSONObjIterator i(*this);
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp(e.fieldName(), name) == 0 )
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

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(DateNowLabeler& id){
        _builder->appendDate(_fieldName, jsTime());
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
        const char * s = b.buf() + offset_;
        const char * e = b.buf() + b.len();
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

}
