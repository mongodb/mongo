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

#pragma once

#include <vector>
#include <set>
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Internal class used by BtreeAccessMethod to generate keys for indexed documents.
 * This class is meant to be kept under the index access layer.
 */
class BtreeKeyGenerator {
public:
    BtreeKeyGenerator(std::vector<const char*> fieldNames,
                      std::vector<BSONElement> fixed,
                      bool isSparse);

    virtual ~BtreeKeyGenerator() {}

    void getKeys(const BSONObj& obj, BSONObjSet* keys) const;

    static const int ParallelArraysCode;

protected:
    // These are used by the getKeysImpl(s) below.
    std::vector<const char*> _fieldNames;
    bool _isIdIndex;
    bool _isSparse;
    BSONObj _nullKey;  // a full key with all fields null
    BSONSizeTracker _sizeTracker;

private:
    // We have V0 and V1.  Sigh.
    virtual void getKeysImpl(std::vector<const char*> fieldNames,
                             std::vector<BSONElement> fixed,
                             const BSONObj& obj,
                             BSONObjSet* keys) const = 0;

    std::vector<BSONElement> _fixed;
};

class BtreeKeyGeneratorV0 : public BtreeKeyGenerator {
public:
    BtreeKeyGeneratorV0(std::vector<const char*> fieldNames,
                        std::vector<BSONElement> fixed,
                        bool isSparse);

    virtual ~BtreeKeyGeneratorV0() {}

private:
    virtual void getKeysImpl(std::vector<const char*> fieldNames,
                             std::vector<BSONElement> fixed,
                             const BSONObj& obj,
                             BSONObjSet* keys) const;
};

class BtreeKeyGeneratorV1 : public BtreeKeyGenerator {
public:
    BtreeKeyGeneratorV1(std::vector<const char*> fieldNames,
                        std::vector<BSONElement> fixed,
                        bool isSparse);

    virtual ~BtreeKeyGeneratorV1() {}

private:
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

        // If we find a positionally indexed element, we traverse the remainder of the path
        // until we find either another array element or the end of the path. The result of
        // this traversal (implemented using getFieldDottedOrArray()), is stored here and used
        // during the recursive call for each array element.
        //
        // Example:
        //   Suppose we have key pattern {"a.1.b.0.c": 1}. The document for which we are
        //   generating keys is {a: [0, {b: [{c: 99}]}]}. We will find that {b: [{c: 99}]}
        //   is a positionally indexed element and store it as 'positionallyIndexedElt'.
        //
        //   We then call getFieldDottedOrArray() to traverse the remainder of the path,
        //   "b.1.c". The result is the array [{c: 99}] which is stored here as 'dottedElt'.
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
     * @param fieldNames - fields to index, may be postfixes in recursive calls
     * @param fixed - values that have already been identified for their index fields
     * @param obj - object from which keys should be extracted, based on names in fieldNames
     * @param keys - set where index keys are written
     * @param numNotFound - number of index fields that have already been identified as missing
     * @param array - array from which keys should be extracted, based on names in fieldNames
     *        If obj and array are both nonempty, obj will be one of the elements of array.
     */
    virtual void getKeysImpl(std::vector<const char*> fieldNames,
                             std::vector<BSONElement> fixed,
                             const BSONObj& obj,
                             BSONObjSet* keys) const;

    /**
     * This recursive method does the heavy-lifting for getKeysImpl().
     */
    void getKeysImplWithArray(std::vector<const char*> fieldNames,
                              std::vector<BSONElement> fixed,
                              const BSONObj& obj,
                              BSONObjSet* keys,
                              unsigned numNotFound,
                              const std::vector<PositionalPathInfo>& positionalInfo) const;
    /**
     * A call to getKeysImplWithArray() begins by calling this for each field in the key
     * pattern. It uses getFieldDottedOrArray() to traverse the path '*field' in 'obj'.
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
     *   getKeysImplWithArray() for each array element. For instance, it will get called with
     *   'obj' {b: {c: 98}} and '*field' pointing to "b.c". It will return element 98 and
     *   set '*field' to "". Similarly, it will return elemtn 99 and set '*field' to "" for
     *   the second array element.
     */
    BSONElement extractNextElement(const BSONObj& obj,
                                   const PositionalPathInfo& positionalInfo,
                                   const char** field,
                                   bool* arrayNestedArray) const;

    /**
     * Sets extracted elements in 'fixed' for field paths that we have traversed to the end.
     *
     * Then calls getKeysImplWithArray() recursively.
     */
    void _getKeysArrEltFixed(std::vector<const char*>* fieldNames,
                             std::vector<BSONElement>* fixed,
                             const BSONElement& arrEntry,
                             BSONObjSet* keys,
                             unsigned numNotFound,
                             const BSONElement& arrObjElt,
                             const std::set<unsigned>& arrIdxs,
                             bool mayExpandArrayUnembedded,
                             const std::vector<PositionalPathInfo>& positionalInfo) const;

    const std::vector<PositionalPathInfo> _emptyPositionalInfo;
};

}  // namespace mongo
