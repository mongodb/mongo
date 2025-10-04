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


#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"

#include <cstddef>
#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

class ElementPath {
public:
    /**
     * Controls how the element path interacts with leaf arrays, e.g. how we will handle the path
     * "a.b" when "b" is an array.
     */
    enum class LeafArrayBehavior {
        // Matches against the elements of arrays at the end of the path (in addition to the array
        // as a whole).
        //
        // For example, for the path "f" and document {f: [1, 2]}, causes the path iterator to
        // return 1, 2, and [1, 2].
        kTraverse,

        // Does not traverse arrays at the end of the path. For the path "f" and document {f: [1,
        // 2]}, the path iterator returns only the entire array [1, 2].
        kNoTraversal,

        // Matches against the elements of arrays at the end of the path. The same as kTraverse, but
        // does not match the array as a whole.
        //
        // For example, for the path "f" and document {f: [1, 2]}, causes the path iterator to
        // return 1 and 2.
        kTraverseOmitArray,
    };

    /**
     * Controls how the element path interacts with non-leaf arrays, e.g. how we will handle the
     * path "a.b" when "a" is an array.
     */
    enum class NonLeafArrayBehavior {
        // Path traversal spans non-leaf arrays.
        kTraverse,

        // Path traversal stops at non-leaf array boundaries. The path iterator will return no
        // elements.
        kNoTraversal,

        // Path traversal stops at non-leaf array boundaries. The path iterator will return the
        // array element.
        kMatchSubpath,
    };

    ElementPath(StringData path,
                LeafArrayBehavior leafArrayBehavior = LeafArrayBehavior::kTraverse,
                NonLeafArrayBehavior nonLeafArrayBehavior = NonLeafArrayBehavior::kTraverse)
        : _leafArrayBehavior(leafArrayBehavior),
          _nonLeafArrayBehavior(nonLeafArrayBehavior),
          _fieldRef(path) {}

    /**
     * Resets this ElementPath to 'newPath'. Note that this method will make a copy of 'newPath'
     * such that there's no lifetime requirements for the string which 'newPath' points into.
     */
    void reset(StringData newPath) {
        _fieldRef.parse(newPath);
    }

    void setLeafArrayBehavior(LeafArrayBehavior leafArrBehavior) {
        _leafArrayBehavior = leafArrBehavior;
    }

    LeafArrayBehavior leafArrayBehavior() const {
        return _leafArrayBehavior;
    }

    void setNonLeafArrayBehavior(NonLeafArrayBehavior value) {
        _nonLeafArrayBehavior = value;
    }

    NonLeafArrayBehavior nonLeafArrayBehavior() const {
        return _nonLeafArrayBehavior;
    }

    const FieldRef& fieldRef() const {
        return _fieldRef;
    }

private:
    LeafArrayBehavior _leafArrayBehavior;
    NonLeafArrayBehavior _nonLeafArrayBehavior;

    FieldRef _fieldRef;
};

class ElementIterator {
public:
    class Context {
    public:
        void reset();

        void reset(BSONElement element, BSONElement arrayOffset);

        void setArrayOffset(BSONElement e) {
            _arrayOffset = e;
        }

        BSONElement element() const {
            return _element;
        }
        BSONElement arrayOffset() const {
            return _arrayOffset;
        }

    private:
        BSONElement _element;
        BSONElement _arrayOffset;
    };

    virtual ~ElementIterator();

    virtual bool more() = 0;
    virtual Context next() = 0;
};

// ---------------------------------------------------------------

class SingleElementElementIterator : public ElementIterator {
public:
    explicit SingleElementElementIterator(BSONElement e) : _seen(false) {
        _element.reset(e, BSONElement());
    }

    ~SingleElementElementIterator() override {}

    bool more() override {
        return !_seen;
    }
    Context next() override {
        _seen = true;
        return _element;
    }

private:
    bool _seen;
    ElementIterator::Context _element;
};

class SimpleArrayElementIterator : public ElementIterator {
public:
    SimpleArrayElementIterator(const BSONElement& theArray, bool returnArrayLast);

    bool more() override;
    Context next() override;

private:
    BSONElement _theArray;
    bool _returnArrayLast;
    BSONObjIterator _iterator;
};

struct BSONElementSubIterator;

class BSONElementIterator : public ElementIterator {
public:
    BSONElementIterator();

    /**
     * Constructs an iterator over 'elementToIterate', where the desired element(s) is/are at the
     * end of the suffix of 'path' starting at 'suffixIndex'. For example, constructing a
     * BSONElementIterator with path="a.b.c", suffixIndex=1, and 'elementToIterate' as the
     * subdocument located at 'a' within the object {a: {b: [{c: 1}, {c: 2}]}} would iterate over
     * the elements of {b: [{c: 1}, {c: 2}]} at the end of the path 'b.c'. 'elementToIterate' does
     * not need to be of type Object, so it would also be valid to construct a BSONElementIterator
     * with path="a.b" and 'elementToIterate' as the array within 'a.b'.
     */
    BSONElementIterator(const ElementPath* path, size_t suffixIndex, BSONElement elementToIterate);

    /**
     * Constructs an iterator over 'objectToIterate', where the desired element(s) is/are at the end
     * of 'path'.
     */
    BSONElementIterator(const ElementPath* path, const BSONObj& objectToIterate);

    ~BSONElementIterator() override;

    void reset(const ElementPath* path, size_t suffixIndex, BSONElement elementToIterate);
    void reset(const ElementPath* path, const BSONObj& objectToIterate);

    bool more() override;
    Context next() override;

private:
    /**
     * Helper for more().  Recurs on _subCursor (which traverses the remainder of a path through
     * subdocuments of an array).
     */
    bool subCursorHasMore();

    /**
     * Sets _traversalStart and _traversalStartIndex by traversing 'elementToIterate' along the
     * suffix of '_path' starting at 'suffixIndex'.
     */
    void _setTraversalStart(size_t suffixIndex, BSONElement elementToIterate);

    const ElementPath* _path;

    // The element where we begin our iteration. This is either:
    // -- The element at the end of _path.
    // -- The first array element encountered along _path.
    // -- EOO, if _path does not exist in the object/element we are exploring.
    BSONElement _traversalStart;

    // This index of _traversalStart in _path, or 0 if _traversalStart is EOO.
    size_t _traversalStartIndex;

    enum State { BEGIN, IN_ARRAY, DONE } _state;
    Context _next;

    struct ArrayIterationState {
        void reset(const FieldRef& ref, int start);
        void startIterator(BSONElement theArray);

        bool more();
        BSONElement next();

        bool isArrayOffsetMatch(StringData fieldName) const;
        bool nextEntireRest() const {
            return nextPieceOfPath.size() == restOfPath.size();
        }

        std::string restOfPath;
        StringData nextPieceOfPath;
        bool hasMore;
        bool nextPieceOfPathIsNumber;

        BSONElement _theArray;
        BSONElement _current;
        boost::optional<BSONObjIterator> _iterator;
    };

    ArrayIterationState _arrayIterationState;

    // Pointer to optional. The optional is used for a convenient API to re-use the memory between
    // instances. Need to wrap with pointer to break cycle when we recursively contain member to
    // self.
    std::unique_ptr<boost::optional<BSONElementSubIterator>> _subIterator;
};

struct BSONElementSubIterator {
    BSONElementSubIterator(const BSONObj& objectToIterate,
                           StringData pathToIterate,
                           ElementPath::LeafArrayBehavior leafArrayBehavior =
                               ElementPath::LeafArrayBehavior::kTraverse,
                           ElementPath::NonLeafArrayBehavior nonLeafArrayBehavior =
                               ElementPath::NonLeafArrayBehavior::kTraverse);

    ElementPath path;
    BSONElementIterator cursor;
};

}  // namespace mongo
