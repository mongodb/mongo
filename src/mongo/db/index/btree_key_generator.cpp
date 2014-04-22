/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/index/btree_key_generator.h"

#include <boost/algorithm/string.hpp>
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    BtreeKeyGenerator::BtreeKeyGenerator(vector<const char*> fieldNames, vector<BSONElement> fixed, 
                                         bool isSparse)
        : _fieldNames(fieldNames), _isSparse(isSparse), _fixed(fixed) {

        BSONObjBuilder nullKeyBuilder;
        for (size_t i = 0; i < fieldNames.size(); ++i) {
            nullKeyBuilder.appendNull("");
        }
        _nullKey = nullKeyBuilder.obj();

        BSONObjBuilder nullEltBuilder;
        nullEltBuilder.appendNull("");
        _nullObj = nullEltBuilder.obj();
        _nullElt = _nullObj.firstElement();
    }

    Status BtreeKeyGenerator::getKeys(const BSONObj &obj, BSONObjSet *keys) const {
        // These are mutated as part of the getKeys call.  :|
        vector<const char*> fieldNames(_fieldNames);
        vector<BSONElement> fixed(_fixed);
        Status s = getKeysImpl(fieldNames, fixed, obj, keys);
        if (!s.isOK()) {
            return s;
        }

        if (keys->empty() && ! _isSparse) {
            keys->insert(_nullKey);
        }
        return s;
    }

    BtreeKeyGeneratorV0::BtreeKeyGeneratorV0(vector<const char*> fieldNames,
                                             vector<BSONElement> fixed, bool isSparse)
            : BtreeKeyGenerator(fieldNames, fixed, isSparse) { }

    Status BtreeKeyGeneratorV0::getKeysImpl(vector<const char*> fieldNames,
                                            vector<BSONElement> fixed,
                                            const BSONObj &obj,
                                            BSONObjSet *keys) const {
        BSONElement arrElt;
        unsigned arrIdx = ~0;
        unsigned numNotFound = 0;

        for ( unsigned i = 0; i < fieldNames.size(); ++i ) {
            if ( *fieldNames[ i ] == '\0' )
                continue;

            BSONElement e = obj.getFieldDottedOrArray( fieldNames[ i ] );

            if ( e.eoo() ) {
                e = _nullElt; // no matching field
                numNotFound++;
            }

            if ( e.type() != Array )
                fieldNames[ i ] = ""; // no matching field or non-array match

            if ( *fieldNames[ i ] == '\0' )
                // no need for further object expansion (though array expansion still possible)
                fixed[ i ] = e;

            if ( e.type() == Array && arrElt.eoo() ) {
                // we only expand arrays on a single path -- track the path here
                arrIdx = i;
                arrElt = e;
            }

            // enforce single array path here
            if ( e.type() == Array && e.rawdata() != arrElt.rawdata() ) {
                mongoutils::str::stream ss;
                ss << "cannot index parallel arrays [" << e.fieldName()
                   << "] [" << arrElt.fieldName() << "]";
                return Status( ErrorCodes::BadValue, ss );
            }
        }

        bool allFound = true; // have we found elements for all field names in the key spec?
        for (vector<const char*>::const_iterator i = fieldNames.begin(); i != fieldNames.end();
             ++i ) {
            if ( **i != '\0' ) {
                allFound = false;
                break;
            }
        }

        if ( _isSparse && numNotFound == _fieldNames.size()) {
            // we didn't find any fields
            // so we're not going to index this document
            return Status::OK();
        }

        bool insertArrayNull = false;

        if ( allFound ) {
            if ( arrElt.eoo() ) {
                // no terminal array element to expand
                BSONObjBuilder b(_sizeTracker);
                for( vector< BSONElement >::iterator i = fixed.begin(); i != fixed.end(); ++i )
                    b.appendAs( *i, "" );
                keys->insert( b.obj() );
            }
            else {
                // terminal array element to expand, so generate all keys
                BSONObjIterator i( arrElt.embeddedObject() );
                if ( i.more() ) {
                    while( i.more() ) {
                        BSONObjBuilder b(_sizeTracker);
                        for( unsigned j = 0; j < fixed.size(); ++j ) {
                            if ( j == arrIdx )
                                b.appendAs( i.next(), "" );
                            else
                                b.appendAs( fixed[ j ], "" );
                        }
                        keys->insert( b.obj() );
                    }
                }
                else if ( fixed.size() > 1 ) {
                    insertArrayNull = true;
                }
            }
        }
        else {
            // nonterminal array element to expand, so recurse
            verify( !arrElt.eoo() );
            BSONObjIterator i( arrElt.embeddedObject() );
            if ( i.more() ) {
                while( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() == Object ) {
                        getKeysImpl( fieldNames, fixed, e.embeddedObject(), keys );
                    }
                }
            }
            else {
                insertArrayNull = true;
            }
        }

        if ( insertArrayNull ) {
            // x : [] - need to insert undefined
            BSONObjBuilder b(_sizeTracker);
            for( unsigned j = 0; j < fixed.size(); ++j ) {
                if ( j == arrIdx ) {
                    b.appendUndefined( "" );
                }
                else {
                    BSONElement e = fixed[j];
                    if ( e.eoo() )
                        b.appendNull( "" );
                    else
                        b.appendAs( e , "" );
                }
            }
            keys->insert( b.obj() );
        }

        return Status::OK();
    }

    BtreeKeyGeneratorV1::BtreeKeyGeneratorV1(vector<const char*> fieldNames,
                                             vector<BSONElement> fixed,
                                             bool isSparse)
        : BtreeKeyGenerator(fieldNames, fixed, isSparse) {
        // Initialize '_keyPattern' by splitting each field name on "." into
        // the component pieces of the path.
        for (size_t i = 0; i < fieldNames.size(); ++i) {
            vector<string> pathPieces;
            boost::split(pathPieces, fieldNames[i], boost::is_any_of("."));
            _keyPattern.push_back(pathPieces);
        }

        BSONObjBuilder undefinedBuilder;
        undefinedBuilder.appendUndefined("");
        _undefinedObj = undefinedBuilder.obj();
        _undefinedElt = _undefinedObj.firstElement();
    }

    Status BtreeKeyGeneratorV1::getKeysImpl(vector<const char*> fieldNames,
                                            vector<BSONElement> fixed,
                                            const BSONObj &obj,
                                            BSONObjSet *keys) const {
        // We may be able to reject right off the bat due to parallel arrays.
        Status parallelStatus = checkForParallelArrays(obj, 0);
        if (!parallelStatus.isOK()) {
            return parallelStatus;
        }

        // Extract key elements for each field in the key pattern, one at a time. The results
        // will be stored in 'kel'.
        KeyElementList kel;
        for (size_t i = 0; i < _keyPattern.size(); ++i) {
            const vector<string>& pathPieces = _keyPattern[i];
            const string& field = pathPieces[0];
            BSONElement el = obj[field];

            // Setup the initial context.
            KeygenContext context;
            context.kpIndex = i;
            context.pathPosition = 0;
            context.inArray = false;

            // Extract elements and add the results to 'kel'.
            vector<BSONElement> keysForElement;
            Status s = getKeysForElement(el, context, &keysForElement);
            if (!s.isOK()) {
                return s;
            }
            kel.push_back(keysForElement);
        }

        // Actually build the index key BSON objects based on the extracted key elements.
        buildKeyObjects(kel, keys);

        return Status::OK();
    }

    Status BtreeKeyGeneratorV1::getKeysForElement(const BSONElement& el,
                                                  KeygenContext context,
                                                  vector<BSONElement>* out) const {
        if (Object == el.type()) {
            // The element is a nested object.
            if (lastPathPosition(context)) {
                // We have reached the final position in the path. The nested object
                // itself will be the index key.
                return getKeysForSimpleElement(el, context, out);
            }
            else {
                // We haven't reached the final position in the path yet. We need
                // to descend further down.
                context.pathPosition++;
                BSONObj subobj = el.Obj();
                Status s = checkForParallelArrays(subobj, context.pathPosition);
                if (!s.isOK()) {
                    return s;
                }

                // Get the indexed element from the nested object.
                const vector<string>& pathPieces = _keyPattern[context.kpIndex];
                const string& field = pathPieces[context.pathPosition];
                BSONElement subEl = subobj[field];

                // Recursively extract keys.
                context.inArray = false;
                return getKeysForElement(subEl, context, out);
            }
        }
        else if (Array == el.type()) {
            return getKeysForArrayElement(el, context, out);
        }
        else {
            // The element is neither an array nor an object.
            return getKeysForSimpleElement(el, context, out);
        }
    }

    Status BtreeKeyGeneratorV1::getKeysForArrayElement(const BSONElement& el,
                                                       KeygenContext context,
                                                       vector<BSONElement>* out) const {
        invariant(Array == el.type());

        // First handle the special case of positional key patterns, e.g. {'a.0': 1}.
        bool isPositional;
        Status posStatus = handlePositionalKeypattern(el, context, &isPositional, out);
        if (isPositional) {
            return posStatus;
        }

        BSONObjIterator it(el.Obj());

        if (!it.more() || context.inArray) {
            // We don't extract keys for empty arrays or arrays nested inside arrays.
            // Rather, these are treated as simple elements.
            return getKeysForSimpleElement(el, context, out);
        }
        else {
            // Extract keys for each element inside the array.
            while (it.more()) {
                BSONElement subEl = it.next();
                context.inArray = true;
                Status s = getKeysForElement(subEl, context, out);
                if (!s.isOK()) {
                    return s;
                }
            }
            return Status::OK();
        }
    }

    Status BtreeKeyGeneratorV1::handlePositionalKeypattern(const BSONElement& el,
                                                           KeygenContext context,
                                                           bool* isPositional,
                                                           vector<BSONElement>* out) const {
        *isPositional = true;

        const vector<string>& pathPieces = _keyPattern[context.kpIndex];

        if (lastPathPosition(context)) {
            // We've already reached the end of the path; there cannot be an additional
            // positional element to handle.
            *isPositional = false;
            return Status::OK();
        }

        // Convert the array into an object with field names '0', '1', '2', ...
        BSONObj arrayObj = el.Obj();

        // Get the name of the potential positional element.
        context.pathPosition++;
        const string& field = pathPieces[context.pathPosition];

        if (arrayObj[field].eoo()) {
            // Either 'field' is not positional, or the indexed position is not
            // present in the array. Bail out now.
            *isPositional = false;
            return Status::OK();
        }

        // We have an element indexed by virtue of its position in the array.
        BSONElement indexedElement = arrayObj[field];

        // Reject the document if it has subobjects with numerically named fields,
        // as this leads to an ambiguity.
        //
        // Ex.
        //   Key pattern {"a.0": 1} and document {"a": [{"0": "foo"}]}. Should the
        //   index key be {"": {"0": "foo:}} or should it be {"": "foo"}?
        BSONObjIterator objIt(arrayObj);
        while (objIt.more()) {
            BSONElement innerElement = objIt.next();
            if (Object == innerElement.type()) {
                BSONObj innerObj = innerElement.Obj();
                if (!innerObj[field].eoo()) {
                    mongoutils::str::stream ss;
                    ss << "Ambiguous field name found in array (do not use numeric field "
                       << "names in embedded elements in an array), field: '" << field
                       << "' for array " << el.toString();
                    return Status(ErrorCodes::BadValue, ss);
                }
            }
        }

        if (lastPathPosition(context)) {
            return getKeysForSimpleElement(indexedElement, context, out);
        }
        else {
            context.inArray = false;
            return getKeysForElement(indexedElement, context, out);
        }
    }

    Status BtreeKeyGeneratorV1::getKeysForSimpleElement(const BSONElement& el,
                                                        KeygenContext context,
                                                        vector<BSONElement>* out) const {
        if (el.eoo() || !lastPathPosition(context)) {
            // Special case: either the indexed element is missing or we never reached the
            // final position in the indexed path. In these cases the index key is {"": null}.
            // If the index is sparse, then there is no index key.
            if (!_isSparse) {
                out->push_back(_nullElt);
            }
            return Status::OK();
        }
        else if (Array == el.type()) {
            BSONObjIterator it(el.Obj());
            if (!it.more()) {
                // Special case: the index key for an empty array is {"": undefined}.
                out->push_back(_undefinedElt);
                return Status::OK();
            }
        }

        // We haven't hit any special cases. The index key is just the element itself.
        out->push_back(el);

        return Status::OK();
    }

    void BtreeKeyGeneratorV1::buildKeyObjects(const KeyElementList& kel, BSONObjSet *keys) const {
        // A list of indexes into 'kel'. Each index in the list points to the BSONElement that
        // we will use next for the corresponding field.
        vector<size_t> posList;

        // The number of index keys to build.
        size_t numKeys = 0;

        // Initialize 'posList' and 'numKeys'.
        for (size_t i = 0; i < kel.size(); ++i) {
            posList.push_back(0);
            if (kel[i].size() > numKeys) {
                numKeys = kel[i].size();
            }
        }

        for (size_t i = 0; i < numKeys; i++) {
            BSONObjBuilder bob;
            for (size_t j = 0; j < posList.size(); j++) {
                size_t pos = posList[j];

                if (pos >= kel[j].size()) {
                    // We don't have an element for this position in the key pattern, so
                    // we append null.
                    bob.appendNull("");
                }
                else {
                    bob.appendAs(kel[j][pos], "");
                    if (kel[j].size() > 1) {
                        ++posList[j];
                    }
                }
            }

            // Output the resulting index key.
            keys->insert(bob.obj());
        }
    }

    //
    // Helpers.
    //

    Status BtreeKeyGeneratorV1::checkForParallelArrays(const BSONObj& obj,
                                                       size_t pathPosition) const {
        const char* knownArray = NULL;
        for (size_t i = 0; i < _keyPattern.size(); ++i) {
            const vector<string>& pathPieces = _keyPattern[i];

            if (pathPosition >= pathPieces.size()) {
                continue;
            }

            const string& field = pathPieces[pathPosition];
            BSONElement el = obj[field];
            if (Array == el.type()) {
                if (NULL != knownArray && !mongoutils::str::equals(knownArray, el.fieldName())) {
                    mongoutils::str::stream ss;
                    ss << "cannot index parallel arrays [" << knownArray
                       << "] [" << el.fieldName() << "]";
                    return Status(ErrorCodes::BadValue, ss);
                }

                knownArray = el.fieldName();
            }
        }

        return Status::OK();
    }

    bool BtreeKeyGeneratorV1::lastPathPosition(const KeygenContext& context) const {
        const vector<string>& pathPieces = _keyPattern[context.kpIndex];
        return (context.pathPosition == (pathPieces.size() - 1));
    }

}  // namespace mongo
