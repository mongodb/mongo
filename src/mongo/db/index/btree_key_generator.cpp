/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/index/btree_key_generator.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

using IndexVersion = IndexDescriptor::IndexVersion;

namespace dps = ::mongo::dotted_path_support;

namespace {

const BSONObj nullObj = BSON("" << BSONNULL);
const BSONElement nullElt = nullObj.firstElement();
const BSONObj undefinedObj = BSON("" << BSONUndefined);
const BSONElement undefinedElt = undefinedObj.firstElement();

}  // namespace

BtreeKeyGenerator::BtreeKeyGenerator(std::vector<const char*> fieldNames,
                                     std::vector<BSONElement> fixed,
                                     bool isSparse,
                                     const CollatorInterface* collator,
                                     KeyString::Version keyStringVersion,
                                     Ordering ordering)
    : _keyStringVersion(keyStringVersion),
      _ordering(ordering),
      _fieldNames(fieldNames),
      _isIdIndex(fieldNames.size() == 1 && std::string("_id") == fieldNames[0]),
      _isSparse(isSparse),
      _nullKeyString(_buildNullKeyString()),
      _fixed(fixed),
      _emptyPositionalInfo(fieldNames.size()),
      _collator(collator) {

    for (const char* fieldName : fieldNames) {
        size_t pathLength = FieldRef{fieldName}.numParts();
        invariant(pathLength > 0);
        _pathLengths.push_back(pathLength);
    }
}

static void assertParallelArrays(const char* first, const char* second) {
    std::stringstream ss;
    ss << "cannot index parallel arrays [" << first << "] [" << second << "]";
    uasserted(ErrorCodes::CannotIndexParallelArrays, ss.str());
}

BSONElement BtreeKeyGenerator::_extractNextElement(const BSONObj& obj,
                                                   const PositionalPathInfo& positionalInfo,
                                                   const char** field,
                                                   bool* arrayNestedArray) const {
    StringData firstField = str::before(*field, '.');
    bool haveObjField = !obj.getField(firstField).eoo();
    BSONElement arrField = positionalInfo.positionallyIndexedElt;

    // An index component field name cannot exist in both a document
    // array and one of that array's children.
    uassert(
        16746,
        str::stream() << "Ambiguous field name found in array (do not use numeric field names in "
                         "embedded elements in an array), field: '"
                      << arrField.fieldName() << "' for array: " << positionalInfo.arrayObj,
        !haveObjField || !positionalInfo.hasPositionallyIndexedElt());

    *arrayNestedArray = false;
    if (haveObjField) {
        return dps::extractElementAtPathOrArrayAlongPath(obj, *field);
    } else if (positionalInfo.hasPositionallyIndexedElt()) {
        if (arrField.type() == Array) {
            *arrayNestedArray = true;
        }
        *field = positionalInfo.remainingPath;
        return positionalInfo.dottedElt;
    }
    return BSONElement();
}

void BtreeKeyGenerator::_getKeysArrEltFixed(std::vector<const char*>* fieldNames,
                                            std::vector<BSONElement>* fixed,
                                            const BSONElement& arrEntry,
                                            KeyStringSet* keys,
                                            unsigned numNotFound,
                                            const BSONElement& arrObjElt,
                                            const std::set<size_t>& arrIdxs,
                                            bool mayExpandArrayUnembedded,
                                            const std::vector<PositionalPathInfo>& positionalInfo,
                                            MultikeyPaths* multikeyPaths,
                                            boost::optional<RecordId> id) const {
    // Set up any terminal array values.
    for (const auto idx : arrIdxs) {
        if (*(*fieldNames)[idx] == '\0') {
            (*fixed)[idx] = mayExpandArrayUnembedded ? arrEntry : arrObjElt;
        }
    }

    // Recurse.
    _getKeysWithArray(*fieldNames,
                      *fixed,
                      arrEntry.type() == Object ? arrEntry.embeddedObject() : BSONObj(),
                      keys,
                      numNotFound,
                      positionalInfo,
                      multikeyPaths,
                      id);
}

void BtreeKeyGenerator::getKeys(const BSONObj& obj,
                                KeyStringSet* keys,
                                MultikeyPaths* multikeyPaths,
                                boost::optional<RecordId> id) const {
    if (_isIdIndex) {
        // we special case for speed
        BSONElement e = obj["_id"];
        if (e.eoo()) {
            keys->insert(_nullKeyString);
        } else if (_collator) {
            BSONObjBuilder b;
            CollationIndexKey::collationAwareIndexKeyAppend(e, _collator, &b);

            KeyString::Builder keyString(_keyStringVersion, b.obj(), _ordering);
            if (id) {
                keyString.appendRecordId(*id);
            }
            /*
             * Insert a copy so its buffer size fits the key size.
             */
            keys->insert(keyString.getValueCopy());
        } else {
            int size = e.size() + 5 /* bson over head*/ - 3 /* remove _id string */;
            BSONObjBuilder b(size);
            b.appendAs(e, "");
            KeyString::Builder keyString(_keyStringVersion, b.obj(), _ordering);
            if (id) {
                keyString.appendRecordId(*id);
            }
            /*
             * Insert a copy so its buffer size fits the key size.
             */
            keys->insert(keyString.getValueCopy());
        }

        // The {_id: 1} index can never be multikey because the _id field isn't allowed to be an
        // array value. We therefore always set 'multikeyPaths' as [ [ ] ].
        if (multikeyPaths) {
            multikeyPaths->resize(1);
        }
    } else {
        if (multikeyPaths) {
            invariant(multikeyPaths->empty());
            multikeyPaths->resize(_fieldNames.size());
        }
        // '_fieldNames' and '_fixed' are passed by value so that their copies can be mutated as
        // part of the _getKeysWithArray method.
        _getKeysWithArray(
            _fieldNames, _fixed, obj, keys, 0, _emptyPositionalInfo, multikeyPaths, id);
    }
    if (keys->empty() && !_isSparse) {
        keys->insert(_nullKeyString);
    }
}

void BtreeKeyGenerator::_getKeysWithArray(std::vector<const char*> fieldNames,
                                          std::vector<BSONElement> fixed,
                                          const BSONObj& obj,
                                          KeyStringSet* keys,
                                          unsigned numNotFound,
                                          const std::vector<PositionalPathInfo>& positionalInfo,
                                          MultikeyPaths* multikeyPaths,
                                          boost::optional<RecordId> id) const {
    BSONElement arrElt;

    // A set containing the position of any indexed fields in the key pattern that traverse through
    // the 'arrElt' array value.
    std::set<size_t> arrIdxs;

    // A vector with size equal to the number of elements in the index key pattern. Each element in
    // the vector, if initialized, refers to the component within the indexed field that traverses
    // through the 'arrElt' array value. We say that this component within the indexed field
    // corresponds to a path that causes the index to be multikey if the 'arrElt' array value
    // contains multiple elements.
    //
    // For example, consider the index {'a.b': 1, 'a.c'} and the document
    // {a: [{b: 1, c: 'x'}, {b: 2, c: 'y'}]}. The path "a" causes the index to be multikey, so we'd
    // have a std::vector<boost::optional<size_t>>{{0U}, {0U}}.
    //
    // Furthermore, due to how positional key patterns are specified, it's possible for an indexed
    // field to cause the index to be multikey at a different component than another indexed field
    // that also traverses through the 'arrElt' array value. It's then also possible for an indexed
    // field not to cause the index to be multikey, even if it traverses through the 'arrElt' array
    // value, because only a particular element would be indexed.
    //
    // For example, consider the index {'a.b': 1, 'a.b.0'} and the document {a: {b: [1, 2]}}. The
    // path "a.b" causes the index to be multikey, but the key pattern "a.b.0" only indexes the
    // first element of the array, so we'd have a
    // std::vector<boost::optional<size_t>>{{1U}, boost::none}.
    std::vector<boost::optional<size_t>> arrComponents(fieldNames.size());

    bool mayExpandArrayUnembedded = true;
    for (size_t i = 0; i < fieldNames.size(); ++i) {
        if (*fieldNames[i] == '\0') {
            continue;
        }

        bool arrayNestedArray;
        // Extract element matching fieldName[ i ] from object xor array.
        BSONElement e =
            _extractNextElement(obj, positionalInfo[i], &fieldNames[i], &arrayNestedArray);

        if (e.eoo()) {
            // if field not present, set to null
            fixed[i] = nullElt;
            // done expanding this field name
            fieldNames[i] = "";
            numNotFound++;
        } else if (e.type() == Array) {
            arrIdxs.insert(i);
            if (arrElt.eoo()) {
                // we only expand arrays on a single path -- track the path here
                arrElt = e;
            } else if (e.rawdata() != arrElt.rawdata()) {
                // enforce single array path here
                assertParallelArrays(e.fieldName(), arrElt.fieldName());
            }
            if (arrayNestedArray) {
                mayExpandArrayUnembedded = false;
            }
        } else {
            // not an array - no need for further expansion
            fixed[i] = e;
        }
    }

    if (arrElt.eoo()) {
        // No array, so generate a single key.
        if (_isSparse && numNotFound == fieldNames.size()) {
            return;
        }
        BSONObjBuilder b(_sizeTracker);
        for (std::vector<BSONElement>::iterator i = fixed.begin(); i != fixed.end(); ++i) {
            CollationIndexKey::collationAwareIndexKeyAppend(*i, _collator, &b);
        }
        KeyString::HeapBuilder keyString(_keyStringVersion, b.obj(), _ordering);
        if (id) {
            keyString.appendRecordId(*id);
        }
        keys->insert(keyString.release());
    } else if (arrElt.embeddedObject().firstElement().eoo()) {
        // We've encountered an empty array.
        if (multikeyPaths && mayExpandArrayUnembedded) {
            // Any indexed path which traverses through the empty array must be recorded as an array
            // component.
            for (auto i : arrIdxs) {
                // We need to determine which component of the indexed field causes the index to be
                // multikey as a result of the empty array. Indexed empty arrays are considered
                // multikey and may occur mid-path. For instance, the indexed path "a.b.c" has
                // multikey components {0, 1} given the document {a: [{b: []}, {b: 1}]}.
                size_t fullPathLength = _pathLengths[i];
                size_t suffixPathLength = FieldRef{fieldNames[i]}.numParts();
                invariant(suffixPathLength < fullPathLength);
                arrComponents[i] = fullPathLength - suffixPathLength - 1;
            }
        }

        // For an empty array, set matching fields to undefined.
        _getKeysArrEltFixed(&fieldNames,
                            &fixed,
                            undefinedElt,
                            keys,
                            numNotFound,
                            arrElt,
                            arrIdxs,
                            true,
                            _emptyPositionalInfo,
                            multikeyPaths,
                            id);
    } else {
        BSONObj arrObj = arrElt.embeddedObject();

        // For positional key patterns, e.g. {'a.1.b': 1}, we lookup the indexed array element
        // and then traverse the remainder of the field path up front. This prevents us from
        // having to look up the indexed element again on each recursive call (i.e. once per
        // array element).
        std::vector<PositionalPathInfo> subPositionalInfo(fixed.size());
        for (size_t i = 0; i < fieldNames.size(); ++i) {
            const bool fieldIsArray = arrIdxs.find(i) != arrIdxs.end();

            if (*fieldNames[i] == '\0') {
                // We've reached the end of the path.
                if (multikeyPaths && fieldIsArray && mayExpandArrayUnembedded) {
                    // The 'arrElt' array value isn't expanded into multiple elements when the last
                    // component of the indexed field is positional and 'arrElt' contains nested
                    // array values. In all other cases, the 'arrElt' array value may be expanded
                    // into multiple element and can therefore cause the index to be multikey.
                    arrComponents[i] = _pathLengths[i] - 1;
                }
                continue;
            }

            // The earlier call to dps::extractElementAtPathOrArrayAlongPath(..., fieldNames[i])
            // modified fieldNames[i] to refer to the suffix of the path immediately following the
            // 'arrElt' array value. If we haven't reached the end of this indexed field yet, then
            // we must have traversed through 'arrElt'.
            invariant(fieldIsArray);

            StringData part = fieldNames[i];
            part = part.substr(0, part.find('.'));
            subPositionalInfo[i].positionallyIndexedElt = arrObj[part];
            if (subPositionalInfo[i].positionallyIndexedElt.eoo()) {
                // We aren't indexing a particular element of the 'arrElt' array value, so it may be
                // expanded into multiple elements. It can therefore cause the index to be multikey.
                if (multikeyPaths) {
                    // We need to determine which component of the indexed field causes the index to
                    // be multikey as a result of the 'arrElt' array value. Since
                    //
                    //   NumComponents("<pathPrefix>") + NumComponents("<pathSuffix>")
                    //       = NumComponents("<pathPrefix>.<pathSuffix>"),
                    //
                    // we can compute the number of components in a prefix of the indexed field by
                    // subtracting the number of components in the suffix 'fieldNames[i]' from the
                    // number of components in the indexed field '_fieldNames[i]'.
                    //
                    // For example, consider the indexed field "a.b.c" and the suffix "c". The path
                    // "a.b.c" has 3 components and the suffix "c" has 1 component. Subtracting the
                    // latter from the former yields the number of components in the prefix "a.b",
                    // i.e. 2.
                    size_t fullPathLength = _pathLengths[i];
                    size_t suffixPathLength = FieldRef{fieldNames[i]}.numParts();
                    invariant(suffixPathLength < fullPathLength);
                    arrComponents[i] = fullPathLength - suffixPathLength - 1;
                }
                continue;
            }

            // We're indexing an array element by its position. Traverse the remainder of the
            // field path now.
            //
            // Indexing an array element by its position selects a particular element of the
            // 'arrElt' array value when generating keys. It therefore cannot cause the index to be
            // multikey.
            subPositionalInfo[i].arrayObj = arrObj;
            subPositionalInfo[i].remainingPath = fieldNames[i];
            subPositionalInfo[i].dottedElt = dps::extractElementAtPathOrArrayAlongPath(
                arrObj, subPositionalInfo[i].remainingPath);
        }

        // Generate a key for each element of the indexed array.
        for (const auto arrObjElem : arrObj) {
            _getKeysArrEltFixed(&fieldNames,
                                &fixed,
                                arrObjElem,
                                keys,
                                numNotFound,
                                arrElt,
                                arrIdxs,
                                mayExpandArrayUnembedded,
                                subPositionalInfo,
                                multikeyPaths,
                                id);
        }
    }

    // Record multikey path components.
    if (multikeyPaths) {
        for (size_t i = 0; i < arrComponents.size(); ++i) {
            if (auto arrComponent = arrComponents[i]) {
                (*multikeyPaths)[i].insert(*arrComponent);
            }
        }
    }
}

KeyString::Value BtreeKeyGenerator::_buildNullKeyString() const {
    BSONObjBuilder nullKeyBuilder;
    for (size_t i = 0; i < _fieldNames.size(); ++i) {
        nullKeyBuilder.appendNull("");
    }
    KeyString::HeapBuilder nullKeyString(_keyStringVersion, nullKeyBuilder.obj(), _ordering);
    return nullKeyString.release();
}

}  // namespace mongo
