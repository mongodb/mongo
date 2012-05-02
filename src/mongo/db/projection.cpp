// projection.cpp

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

#include "pch.h"
#include "projection.h"
#include "../util/mongoutils/str.h"

namespace mongo {

    void Projection::init( const BSONObj& o ) {
        massert( 10371 , "can only add to Projection once", _source.isEmpty());
        _source = o;

        BSONObjIterator i( o );
        int true_false = -1;
        while ( i.more() ) {
            BSONElement e = i.next();

            if ( ! e.isNumber() )
                _hasNonSimple = true;

            if (e.type() == Object) {
                BSONObj obj = e.embeddedObject();
                BSONElement e2 = obj.firstElement();
                if ( strcmp(e2.fieldName(), "$slice") == 0 ) {
                    if (e2.isNumber()) {
                        int i = e2.numberInt();
                        if (i < 0)
                            add(e.fieldName(), i, -i); // limit is now positive
                        else
                            add(e.fieldName(), 0, i);

                    }
                    else if (e2.type() == Array) {
                        BSONObj arr = e2.embeddedObject();
                        uassert(13099, "$slice array wrong size", arr.nFields() == 2 );

                        BSONObjIterator it(arr);
                        int skip = it.next().numberInt();
                        int limit = it.next().numberInt();
                        uassert(13100, "$slice limit must be positive", limit > 0 );
                        add(e.fieldName(), skip, limit);

                    }
                    else {
                        uassert(13098, "$slice only supports numbers and [skip, limit] arrays", false);
                    }
                }
                else {
                    uassert(13097, string("Unsupported projection option: ") + obj.firstElementFieldName(), false);
                }

            }
            else if (!strcmp(e.fieldName(), "_id") && !e.trueValue()) {
                _includeID = false;

            }
            else {

                add (e.fieldName(), e.trueValue());

                // validate input
                if (true_false == -1) {
                    true_false = e.trueValue();
                    _include = !e.trueValue();
                }
                else {
                    uassert( 10053 , "You cannot currently mix including and excluding fields. Contact us if this is an issue." ,
                             (bool)true_false == e.trueValue() );
                }
            }
        }
    }

    void Projection::add(const string& field, bool include) {
        if (field.empty()) { // this is the field the user referred to
            _include = include;
        }
        else {
            _include = !include;

            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot+1,string::npos));

            boost::shared_ptr<Projection>& fm = _fields[subfield];
            if (!fm)
                fm.reset(new Projection());

            fm->add(rest, include);
        }
    }

    void Projection::add(const string& field, int skip, int limit) {
        _special = true; // can't include or exclude whole object

        if (field.empty()) { // this is the field the user referred to
            _skip = skip;
            _limit = limit;
        }
        else {
            const size_t dot = field.find('.');
            const string subfield = field.substr(0,dot);
            const string rest = (dot == string::npos ? "" : field.substr(dot+1,string::npos));

            boost::shared_ptr<Projection>& fm = _fields[subfield];
            if (!fm)
                fm.reset(new Projection());

            fm->add(rest, skip, limit);
        }
    }

    void Projection::transform( const BSONObj& in , BSONObjBuilder& b ) const {
        BSONObjIterator i(in);
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( mongoutils::str::equals( "_id" , e.fieldName() ) ) {
                if ( _includeID )
                    b.append( e );
            }
            else {
                append( b , e );
            }
        }
    }

    BSONObj Projection::transform( const BSONObj& in ) const {
        BSONObjBuilder b;
        transform( in , b );
        return b.obj();
    }


    //b will be the value part of an array-typed BSONElement
    void Projection::appendArray( BSONObjBuilder& b , const BSONObj& a , bool nested) const {
        int skip  = nested ?  0 : _skip;
        int limit = nested ? -1 : _limit;

        if (skip < 0) {
            skip = max(0, skip + a.nFields());
        }

        int i=0;
        BSONObjIterator it(a);
        while (it.more()) {
            BSONElement e = it.next();

            if (skip) {
                skip--;
                continue;
            }

            if (limit != -1 && (limit-- == 0)) {
                break;
            }

            switch(e.type()) {
            case Array: {
                BSONObjBuilder subb;
                appendArray(subb , e.embeddedObject(), true);
                b.appendArray(b.numStr(i++), subb.obj());
                break;
            }
            case Object: {
                BSONObjBuilder subb;
                BSONObjIterator jt(e.embeddedObject());
                while (jt.more()) {
                    append(subb , jt.next());
                }
                b.append(b.numStr(i++), subb.obj());
                break;
            }
            default:
                if (_include)
                    b.appendAs(e, b.numStr(i++));
            }
        }
    }

    void Projection::append( BSONObjBuilder& b , const BSONElement& e ) const {
        FieldMap::const_iterator field = _fields.find( e.fieldName() );

        if (field == _fields.end()) {
            if (_include)
                b.append(e);
        }
        else {
            Projection& subfm = *field->second;

            if ((subfm._fields.empty() && !subfm._special) || !(e.type()==Object || e.type()==Array) ) {
                if (subfm._include)
                    b.append(e);
            }
            else if (e.type() == Object) {
                BSONObjBuilder subb;
                BSONObjIterator it(e.embeddedObject());
                while (it.more()) {
                    subfm.append(subb, it.next());
                }
                b.append(e.fieldName(), subb.obj());

            }
            else { //Array
                BSONObjBuilder subb;
                subfm.appendArray(subb, e.embeddedObject());
                b.appendArray(e.fieldName(), subb.obj());
            }
        }
    }

    Projection::KeyOnly* Projection::checkKey( const BSONObj& keyPattern ) const {
        if ( _include ) {
            // if we default to including then we can't
            // use an index because we don't know what we're missing
            return 0;
        }

        if ( _hasNonSimple )
            return 0;

        if ( _includeID && keyPattern["_id"].eoo() )
            return 0;

        // at this point we know its all { x : 1 } style

        auto_ptr<KeyOnly> p( new KeyOnly() );

        int got = 0;
        BSONObjIterator i( keyPattern );
        while ( i.more() ) {
            BSONElement k = i.next();

            if ( _source[k.fieldName()].type() ) {

                if ( strchr( k.fieldName() , '.' ) ) {
                    // TODO we currently don't support dotted fields
                    //      SERVER-2104
                    return 0;
                }

                if ( ! _includeID && mongoutils::str::equals( k.fieldName() , "_id" ) ) {
                    p->addNo();
                }
                else {
                    p->addYes( k.fieldName() );
                    got++;
                }
            }
            else if ( mongoutils::str::equals( "_id" , k.fieldName() ) && _includeID ) {
                p->addYes( "_id" );
            }
            else {
                p->addNo();
            }

        }

        int need = _source.nFields();
        if ( ! _includeID )
            need--;

        if ( got == need )
            return p.release();

        return 0;
    }

    BSONObj Projection::KeyOnly::hydrate( const BSONObj& key ) const {
        verify( _include.size() == _names.size() );

        BSONObjBuilder b( key.objsize() + _stringSize + 16 );

        BSONObjIterator i(key);
        unsigned n=0;
        while ( i.more() ) {
            verify( n < _include.size() );
            BSONElement e = i.next();
            if ( _include[n] ) {
                b.appendAs( e , _names[n] );
            }
            n++;
        }

        return b.obj();
    }
}
