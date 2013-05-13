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

#include "mongo/pch.h"

#include "mongo/db/projection.h"

#include "mongo/db/matcher.h"
#include "mongo/util/mongoutils/str.h"

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
                if ( mongoutils::str::equals( e2.fieldName(), "$slice" ) ) {
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
                else if ( mongoutils::str::equals( e2.fieldName(), "$elemMatch" ) ) {
                    // validate $elemMatch arguments and dependencies
                    uassert( 16342, "elemMatch: invalid argument.  object required.",
                             e2.type() == Object );
                    uassert( 16343, "Cannot specify positional operator and $elemMatch"
                                    " (currently unsupported).",
                             _arrayOpType != ARRAY_OP_POSITIONAL );
                    uassert( 16344, "Cannot use $elemMatch projection on a nested field"
                                    " (currently unsupported).",
                             ! mongoutils::str::contains( e.fieldName(), '.' ) );
                    _arrayOpType = ARRAY_OP_ELEM_MATCH;

                    // initialize new Matcher object(s)

                    _matchers[mongoutils::str::before(e.fieldName(), '.').c_str()]
                            = boost::make_shared<Matcher>(e.wrap(), true);
                    add( e.fieldName(), true );
                }
                else {
                    uasserted(13097, string("Unsupported projection option: ") +
                                     obj.firstElementFieldName() );
                }

            }
            else if (!strcmp(e.fieldName(), "_id") && !e.trueValue()) {
                _includeID = false;
            }
            else {
                add( e.fieldName(), e.trueValue() );

                // validate input
                if (true_false == -1) {
                    true_false = e.trueValue();
                    _include = !e.trueValue();
                }
                else {
                    uassert( 10053 , "You cannot currently mix including and excluding fields. "
                                     "Contact us if this is an issue." ,
                             (bool)true_false == e.trueValue() );
                }
            }
            if ( mongoutils::str::contains( e.fieldName(), ".$" ) ) {
                // positional op found; verify dependencies
                uassert( 16345, "Cannot exclude array elements with the positional operator"
                                " (currently unsupported).", e.trueValue() );
                uassert( 16346, "Cannot specify more than one positional array element per query"
                                " (currently unsupported).", _arrayOpType != ARRAY_OP_POSITIONAL );
                uassert( 16347, "Cannot specify positional operator and $elemMatch"
                                " (currently unsupported).", _arrayOpType != ARRAY_OP_ELEM_MATCH );
                _arrayOpType = ARRAY_OP_POSITIONAL;
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

            boost::shared_ptr<Projection>& fm = _fields[subfield.c_str()];
            if (!fm)
                fm = boost::make_shared<Projection>();

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

            boost::shared_ptr<Projection>& fm = _fields[subfield.c_str()];
            if (!fm)
                fm = boost::make_shared<Projection>();

            fm->add(rest, skip, limit);
        }
    }

    void Projection::transform( const BSONObj& in , BSONObjBuilder& b, const MatchDetails* details ) const {
        const ArrayOpType& arrayOpType = getArrayOpType();

        BSONObjIterator i(in);
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( mongoutils::str::equals( "_id" , e.fieldName() ) ) {
                if ( _includeID )
                    b.append( e );
            }
            else {
                Matchers::const_iterator matcher = _matchers.find( e.fieldName() );
                if ( matcher == _matchers.end() ) {
                    // no array projection matchers for this field
                    append( b, e, details, arrayOpType );
                } else {
                    // field has array projection with $elemMatch specified.
                    massert( 16348, "matchers are only supported for $elemMatch", 
                             arrayOpType == ARRAY_OP_ELEM_MATCH );
                    MatchDetails arrayDetails;
                    arrayDetails.requestElemMatchKey();
                    if ( matcher->second->matches( in, &arrayDetails ) ) {
                        LOG(4) << "Matched array on field: " << matcher->first  << endl
                               << " from array: " << in.getField( matcher->first ) << endl
                               << " in object: " << in << endl
                               << " at position: " << arrayDetails.elemMatchKey() << endl;
                        FieldMap::const_iterator field = _fields.find( e.fieldName()  );
                        massert( 16349, "$elemMatch specified, but projection field not found.",
                            field != _fields.end() );
                        BSONArrayBuilder a;
                        BSONObjBuilder o;
                        massert( 16350, "$elemMatch called on document element with eoo",
                                 ! in.getField( e.fieldName() ).eoo() );
                        massert( 16351, "$elemMatch called on array element with eoo",
                                 ! in.getField( e.fieldName() ).Obj().getField(
                                        arrayDetails.elemMatchKey() ).eoo() );
                        a.append( in.getField( e.fieldName() ).Obj()
                                    .getField( arrayDetails.elemMatchKey() ) );
                        o.appendArray( matcher->first, a.arr() );
                        append( b, o.done().firstElement(), details, arrayOpType );
                    }
                }
            }
        }
    }

    BSONObj Projection::transform( const BSONObj& in, const MatchDetails* details ) const {
        BSONObjBuilder b;
        transform( in , b, details );
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

    void Projection::append( BSONObjBuilder& b , const BSONElement& e, const MatchDetails* details,
                             const ArrayOpType arrayOpType ) const {

        FieldMap::const_iterator field = _fields.find( e.fieldName() );
        if (field == _fields.end()) {
            if (_include)
                b.append(e);
        }
        else {
            Projection& subfm = *field->second;
            if ( ( subfm._fields.empty() && !subfm._special ) ||
                 !(e.type()==Object || e.type()==Array) ) {
                // field map empty, or element is not an array/object
                if (subfm._include)
                    b.append(e);
            }
            else if (e.type() == Object) {
                BSONObjBuilder subb;
                BSONObjIterator it(e.embeddedObject());
                while (it.more()) {
                    subfm.append(subb, it.next(), details, arrayOpType);
                }
                b.append(e.fieldName(), subb.obj());
            }
            else { //Array
                BSONObjBuilder matchedBuilder;
                if ( details && arrayOpType == ARRAY_OP_POSITIONAL ) {
                    // $ positional operator specified

                    LOG(4) << "projection: checking if element " << e << " matched spec: "
                           << getSpec() << " match details: " << *details << endl;
                    uassert( 16352, mongoutils::str::stream() << "positional operator ("
                                        << e.fieldName()
                                        << ".$) requires corresponding field in query specifier",
                                   details && details->hasElemMatchKey() );

                    uassert( 16353, "positional operator element mismatch",
                             ! e.embeddedObject()[details->elemMatchKey()].eoo() );

                    // append as the first and only element in the projected array
                    matchedBuilder.appendAs( e.embeddedObject()[details->elemMatchKey()], "0" );
                }
                else {
                    // append exact array; no subarray matcher specified
                    subfm.appendArray( matchedBuilder, e.embeddedObject() );
                }
                b.appendArray( e.fieldName(), matchedBuilder.obj() );
            }
        }
    }

    Projection::ArrayOpType Projection::getArrayOpType( ) const {
        return _arrayOpType;
    }

    void Projection::validateQuery( const BSONObj query ) const {
        // this function only validates positional operator ($) projections
        if ( getArrayOpType() != ARRAY_OP_POSITIONAL )
            return;

        BSONObjIterator querySpecIter( query );
        while ( querySpecIter.more() ) {
            // for each query element

            BSONElement queryElement = querySpecIter.next();
            if ( mongoutils::str::equals( queryElement.fieldName(), "$and" ) )
                // don't check $and to avoid deep comparison of the arguments.
                // TODO: can be replaced with Matcher::FieldSink when complete (SERVER-4644)
                return;

            BSONObjIterator projectionSpecIter( getSpec() );
            while ( projectionSpecIter.more() ) {
                // for each projection element

                BSONElement projectionElement = projectionSpecIter.next();
                if ( mongoutils::str::contains( projectionElement.fieldName(), ".$" ) &&
                        mongoutils::str::before( queryElement.fieldName(), '.' ) ==
                        mongoutils::str::before( projectionElement.fieldName(), "." ) ) {

                    // found query spec that matches positional array projection spec
                    LOG(4) << "Query specifies field named for positional operator: "
                           << queryElement.fieldName() << endl;
                    return;
                }
            }
        }

        uasserted( 16354, "Positional operator does not match the query specifier." );
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
