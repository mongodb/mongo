// path.h

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


#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"

namespace mongo {

class ElementPath {
public:
    Status init(StringData path);

    void setTraverseNonleafArrays(bool b) {
        _shouldTraverseNonleafArrays = b;
    }
    void setTraverseLeafArray(bool b) {
        _shouldTraverseLeafArray = b;
    }

    const FieldRef& fieldRef() const {
        return _fieldRef;
    }
    bool shouldTraverseNonleafArrays() const {
        return _shouldTraverseNonleafArrays;
    }
    bool shouldTraverseLeafArray() const {
        return _shouldTraverseLeafArray;
    }

private:
    FieldRef _fieldRef;
    bool _shouldTraverseNonleafArrays;
    bool _shouldTraverseLeafArray;
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

    virtual ~SingleElementElementIterator() {}

    virtual bool more() {
        return !_seen;
    }
    virtual Context next() {
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

    virtual bool more();
    virtual Context next();

private:
    BSONElement _theArray;
    bool _returnArrayLast;
    BSONObjIterator _iterator;
};

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

    virtual ~BSONElementIterator();

    void reset(const ElementPath* path, size_t suffixIndex, BSONElement elementToIterate);
    void reset(const ElementPath* path, const BSONObj& objectToIterate);

    bool more();
    Context next();

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
        bool hasMore;
        StringData nextPieceOfPath;
        bool nextPieceOfPathIsNumber;

        BSONElement _theArray;
        BSONElement _current;
        std::unique_ptr<BSONObjIterator> _iterator;
    };

    ArrayIterationState _arrayIterationState;

    std::unique_ptr<ElementIterator> _subCursor;
    std::unique_ptr<ElementPath> _subCursorPath;
};
}
