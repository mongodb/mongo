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

#pragma once

#include <memory>
#include <set>
#include <vector>

#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/key_string.h"

namespace mongo {

class CollatorInterface;

/**
 * Internal class used by BtreeAccessMethod to generate keys for indexed documents.
 * This class is meant to be kept under the index access layer.
 */
class BtreeKeyGenerator {
public:
    /**
     * Provides a context to generate keys based on names in 'fieldNames'. The 'fixed' argument
     * specifies values that have already been identified for their corresponding fields.
     */
    BtreeKeyGenerator(std::vector<const char*> fieldNames,
                      std::vector<BSONElement> fixed,
                      bool isSparse,
                      const CollatorInterface* collator,
                      KeyString::Version keyStringVersion,
                      Ordering ordering);

    /**
     * Generates the index keys for the document 'obj', and stores them in the set 'keys'.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     */
    void getKeys(const BSONObj& obj,
                 KeyStringSet* keys,
                 MultikeyPaths* multikeyPaths,
                 boost::optional<RecordId> id = boost::none) const;

private:
    const KeyString::Version _keyStringVersion;
    const Ordering _ordering;
    // These are used by getKeys below.
    const std::vector<const char*> _fieldNames;
    const bool _isIdIndex;
    const bool _isSparse;
    const KeyString::Value _nullKeyString;  // A full key with all fields null.
    const BSONSizeTracker _sizeTracker;

    std::vector<BSONElement> _fixed;
    /**
     * Stores info regarding traversal of a positional path. A path through a document is
     * considered positional if this path element names an array element. Generally this means
     * that the field name consists of [0-9]+, but the implementation just calls .Obj() on
     * the array and looks for the named field. This logic happens even if the field does
     * not match [0-9]+.
     *
     * Example:
     *   The path 'a.1.b' can sometimes be positional due to path element '1'. In the document
     *   {a: [{b: 98}, {b: 99}]} it would be considered positional, and would refer to
     *   element 99. In the document {a: [{'1': {b: 97}}]}, the path is *not* considered
     *   positional and would refer to element 97.
     */
    struct PositionalPathInfo {
        PositionalPathInfo() : remainingPath("") {}

        bool hasPositionallyIndexedElt() const {
            return !positionallyIndexedElt.eoo();
        }

        // Stores the array element indexed by position. If the key pattern has no positional
        // element, then this is EOO.
        //
        // Example:
        //   Suppose the key pattern is {"a.0.x": 1} and we're extracting keys for document
        //   {a: [{x: 98}, {x: 99}]}. We should store element {x: 98} here.
        BSONElement positionallyIndexedElt;

        // The array to which 'positionallyIndexedElt' belongs.
        BSONObj arrayObj;

        // If we find a positionally indexed element, we traverse the remainder of the path until we
        // find either another array element or the end of the path. The result of this traversal is
        // stored here and used during the recursive call for each array element.
        //
        // Example:
        //   Suppose we have key pattern {"a.1.b.0.c": 1}. The document for which we are
        //   generating keys is {a: [0, {b: [{c: 99}]}]}. We will find that {b: [{c: 99}]}
        //   is a positionally indexed element and store it as 'positionallyIndexedElt'.
        //
        //   We then traverse the remainder of the path, "b.0.c", until encountering an array. The
        //   result is the array [{c: 99}] which is stored here as 'dottedElt'.
        BSONElement dottedElt;

        // The remaining path that must be traversed in 'dottedElt' to find the indexed
        // element(s).
        //
        // Example:
        //   Continuing the example above, 'remainingPath' will be "0.c". Note that the path
        //   "0.c" refers to element 99 in 'dottedElt', [{c: 99}].
        const char* remainingPath;
    };

    /**
     * This recursive method does the heavy-lifting for getKeys().
     */
    void _getKeysWithArray(std::vector<const char*> fieldNames,
                           std::vector<BSONElement> fixed,
                           const BSONObj& obj,
                           KeyStringSet* keys,
                           unsigned numNotFound,
                           const std::vector<PositionalPathInfo>& positionalInfo,
                           MultikeyPaths* multikeyPaths,
                           boost::optional<RecordId> id) const;

    /**
     * A call to _getKeysWithArray() begins by calling this for each field in the key pattern. It
     * traverses the path '*field' in 'obj' until either reaching the end of the path or an array
     * element.
     *
     * The 'positionalInfo' arg is used for handling a field path where 'obj' has an
     * array indexed by position. See the comments for PositionalPathInfo for more detail.
     *
     * Returns the element extracted as a result of traversing the path, or an indexed array
     * if we encounter one during the path traversal.
     *
     * Out-parameters:
     *   --Sets *field to the remaining path that must be traversed.
     *   --Sets *arrayNestedArray to true if the returned BSONElement is a nested array that is
     *     indexed by position in its parent array. Otherwise sets *arrayNestedArray to false.
     *
     * Example:
     *   Suppose we have key pattern {"a.b.c": 1} and we're extracting keys from document
     *   {a: [{b: {c: 98}}, {b: {c: 99}}]}. On the first call to extractNextElement(), 'obj'
     *   will be the full document, {a: [{b: {c: 98}}, {b: {c: 99}}]}. The 'positionalInfo'
     *   argument is not relevant, because the array is not being positionally indexed.
     *   '*field' will point to "a.b.c".
     *
     *   The return value will be the array element [{b: {c: 98}}, {b: {c: 99}}], because path
     *   traversal stops when an indexed array is encountered. Furthermore, '*field' will be set
     *   to "b.c".
     *
     *   extractNextElement() will then be called from a recursive call to
     *   _getKeysWithArray() for each array element. For instance, it will get called with
     *   'obj' {b: {c: 98}} and '*field' pointing to "b.c". It will return element 98 and
     *   set '*field' to "". Similarly, it will return elemtn 99 and set '*field' to "" for
     *   the second array element.
     */
    BSONElement _extractNextElement(const BSONObj& obj,
                                    const PositionalPathInfo& positionalInfo,
                                    const char** field,
                                    bool* arrayNestedArray) const;

    /**
     * Sets extracted elements in 'fixed' for field paths that we have traversed to the end.
     *
     * Then calls _getKeysWithArray() recursively.
     */
    void _getKeysArrEltFixed(std::vector<const char*>* fieldNames,
                             std::vector<BSONElement>* fixed,
                             const BSONElement& arrEntry,
                             KeyStringSet* keys,
                             unsigned numNotFound,
                             const BSONElement& arrObjElt,
                             const std::set<size_t>& arrIdxs,
                             bool mayExpandArrayUnembedded,
                             const std::vector<PositionalPathInfo>& positionalInfo,
                             MultikeyPaths* multikeyPaths,
                             boost::optional<RecordId> id) const;

    KeyString::Value _buildNullKeyString() const;

    const std::vector<PositionalPathInfo> _emptyPositionalInfo;

    // A vector with size equal to the number of elements in the index key pattern. Each element in
    // the vector is the number of path components in the indexed field.
    std::vector<size_t> _pathLengths;

    // Null if this key generator orders strings according to the simple binary compare. If
    // non-null, represents the collator used to generate index keys for indexed strings.
    const CollatorInterface* _collator;
};

}  // namespace mongo
