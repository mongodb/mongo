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

#include "mongo/db/matcher/path.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/path_internal.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

// -----

ElementIterator::~ElementIterator() {}

void ElementIterator::Context::reset() {
    _element = BSONElement();
}

void ElementIterator::Context::reset(BSONElement element, BSONElement arrayOffset) {
    _element = element;
    _arrayOffset = arrayOffset;
}

// ------

SimpleArrayElementIterator::SimpleArrayElementIterator(const BSONElement& theArray,
                                                       bool returnArrayLast)
    : _theArray(theArray), _returnArrayLast(returnArrayLast), _iterator(theArray.Obj()) {}

bool SimpleArrayElementIterator::more() {
    return _iterator.more() || _returnArrayLast;
}

ElementIterator::Context SimpleArrayElementIterator::next() {
    if (_iterator.more()) {
        Context e;
        e.reset(_iterator.next(), BSONElement());
        return e;
    }
    _returnArrayLast = false;
    Context e;
    e.reset(_theArray, BSONElement());
    return e;
}


// ------
BSONElementIterator::BSONElementIterator() {
    _path = nullptr;
}

BSONElementIterator::BSONElementIterator(const ElementPath* path,
                                         size_t suffixIndex,
                                         BSONElement elementToIterate)
    : _path(path), _state(BEGIN) {
    _setTraversalStart(suffixIndex, elementToIterate);
}

BSONElementIterator::BSONElementIterator(const ElementPath* path, const BSONObj& objectToIterate)
    : _path(path), _state(BEGIN) {
    _traversalStart =
        getFieldDottedOrArray(objectToIterate, _path->fieldRef(), &_traversalStartIndex);
}

BSONElementIterator::~BSONElementIterator() {}

void BSONElementIterator::reset(const ElementPath* path,
                                size_t suffixIndex,
                                BSONElement elementToIterate) {
    _path = path;
    _traversalStartIndex = 0;
    _traversalStart = BSONElement();
    _setTraversalStart(suffixIndex, elementToIterate);
    _state = BEGIN;
    _next.reset();

    // Reset optional, keep memory around in case we need to instantiate a subIterator again.
    if (_subIterator) {
        _subIterator->reset();
    }
}

void BSONElementIterator::reset(const ElementPath* path, const BSONObj& objectToIterate) {
    _path = path;
    _traversalStartIndex = 0;
    _traversalStart =
        getFieldDottedOrArray(objectToIterate, _path->fieldRef(), &_traversalStartIndex);
    _state = BEGIN;
    _next.reset();

    // Reset optional, keep memory around in case we need to instantiate a subIterator again.
    if (_subIterator) {
        _subIterator->reset();
    }
}

void BSONElementIterator::_setTraversalStart(size_t suffixIndex, BSONElement elementToIterate) {
    invariant(_path->fieldRef().numParts() >= suffixIndex);

    if (suffixIndex == _path->fieldRef().numParts()) {
        _traversalStart = elementToIterate;
    } else {
        if (elementToIterate.type() == BSONType::object) {
            _traversalStart = getFieldDottedOrArray(
                elementToIterate.Obj(), _path->fieldRef(), &_traversalStartIndex, suffixIndex);
        } else if (elementToIterate.type() == BSONType::array) {
            _traversalStart = elementToIterate;
        }
    }
}

void BSONElementIterator::ArrayIterationState::reset(const FieldRef& ref, int start) {
    restOfPath = std::string{ref.dottedField(start)};
    hasMore = restOfPath.size() > 0;
    if (hasMore) {
        nextPieceOfPath = ref.getPart(start);
        nextPieceOfPathIsNumber = str::isAllDigits(nextPieceOfPath);
    } else {
        nextPieceOfPathIsNumber = false;
    }
}

bool BSONElementIterator::ArrayIterationState::isArrayOffsetMatch(StringData fieldName) const {
    if (!nextPieceOfPathIsNumber)
        return false;
    return nextPieceOfPath == fieldName;
}


void BSONElementIterator::ArrayIterationState::startIterator(BSONElement e) {
    _theArray = e;
    _iterator.emplace(_theArray.Obj());
}

bool BSONElementIterator::ArrayIterationState::more() {
    return _iterator && _iterator->more();
}

BSONElement BSONElementIterator::ArrayIterationState::next() {
    _current = _iterator->next();
    return _current;
}


bool BSONElementIterator::subCursorHasMore() {
    // While we still are still finding arrays along the path, keep traversing deeper.
    while (_subIterator && _subIterator->has_value()) {
        if (_subIterator->value().cursor.more()) {
            return true;
        }
        // Reset optional, keep memory around in case we need to instantiate a subIterator again.
        _subIterator->reset();

        // If the subcursor doesn't have more, see if the current element is an array offset
        // match (see comment in BSONElementIterator::more() for an example).  If it is indeed
        // an array offset match, create a new subcursor and examine it.
        if (_arrayIterationState.isArrayOffsetMatch(_arrayIterationState._current.fieldName())) {
            if (_arrayIterationState.nextEntireRest()) {
                // Our path terminates at the array offset.  _next should point at the current
                // array element. _next._arrayOffset should be EOO, since this is not an implicit
                // array traversal.
                _next.reset(_arrayIterationState._current, BSONElement());
                _arrayIterationState._current = BSONElement();
                return true;
            }

            _subIterator->emplace(_arrayIterationState._current.Obj(),
                                  _arrayIterationState.restOfPath.substr(
                                      _arrayIterationState.nextPieceOfPath.size() + 1),
                                  _path->leafArrayBehavior());

            // If we're here, we must be able to traverse nonleaf arrays.
            dassert(_path->nonLeafArrayBehavior() == ElementPath::NonLeafArrayBehavior::kTraverse);
            dassert(_subIterator->value().path.nonLeafArrayBehavior() ==
                    ElementPath::NonLeafArrayBehavior::kTraverse);

            // Set _arrayIterationState._current to EOO. This is not an implicit array traversal, so
            // we should not override the array offset of the subcursor with the current array
            // offset.
            _arrayIterationState._current = BSONElement();
        }
    }

    return false;
}

bool BSONElementIterator::more() {
    if (subCursorHasMore()) {
        return true;
    }

    if (!_next.element().eoo()) {
        return true;
    }

    if (_state == DONE) {
        return false;
    }

    if (_state == BEGIN) {
        if (_traversalStart.type() != BSONType::array) {
            _next.reset(_traversalStart, BSONElement());
            _state = DONE;
            return true;
        }

        // It's an array.

        _arrayIterationState.reset(_path->fieldRef(), _traversalStartIndex + 1);

        if (_arrayIterationState.hasMore &&
            _path->nonLeafArrayBehavior() != ElementPath::NonLeafArrayBehavior::kTraverse) {
            // Don't allow traversing the array.
            if (_path->nonLeafArrayBehavior() == ElementPath::NonLeafArrayBehavior::kMatchSubpath) {
                _next.reset(_traversalStart, BSONElement());
                _state = DONE;
                return true;
            }

            _state = DONE;
            return false;
        } else if (!_arrayIterationState.hasMore &&
                   _path->leafArrayBehavior() == ElementPath::LeafArrayBehavior::kNoTraversal) {
            // Return the leaf array.
            _next.reset(_traversalStart, BSONElement());
            _state = DONE;
            return true;
        }

        _arrayIterationState.startIterator(_traversalStart);
        _state = IN_ARRAY;

        invariant(_next.element().eoo());
    }

    if (_state == IN_ARRAY) {
        // We're traversing an array.  Look at each array element.

        while (_arrayIterationState.more()) {
            BSONElement eltInArray = _arrayIterationState.next();
            if (!_arrayIterationState.hasMore) {
                // Our path terminates at this array.  _next should point at the current array
                // element.
                _next.reset(eltInArray, eltInArray);
                return true;
            }

            // Our path does not terminate at this array; there's a subpath left over.  Inspect
            // the current array element to see if it could match the subpath.

            if (eltInArray.type() == BSONType::object) {
                // The current array element is a subdocument.  See if the subdocument generates
                // any elements matching the remaining subpath.
                if (!_subIterator) {
                    _subIterator = std::make_unique<boost::optional<BSONElementSubIterator>>();
                }
                _subIterator->emplace(
                    eltInArray.Obj(), _arrayIterationState.restOfPath, _path->leafArrayBehavior());

                if (subCursorHasMore()) {
                    return true;
                }
            } else if (_arrayIterationState.isArrayOffsetMatch(eltInArray.fieldName())) {
                // The path we're traversing has an array offset component, and the current
                // array element corresponds to the offset we're looking for (for example: our
                // path has a ".0" component, and we're looking at the first element of the
                // array, so we should look inside this element).

                if (_arrayIterationState.nextEntireRest()) {
                    // Our path terminates at the array offset.  _next should point at the
                    // current array element. _next._arrayOffset should be EOO, since this is not an
                    // implicit array traversal.
                    _next.reset(eltInArray, BSONElement());
                    return true;
                }

                invariant(eltInArray.type() != BSONType::object);  // Handled above.
                if (eltInArray.type() == BSONType::array) {
                    // The current array element is itself an array.  See if the nested array
                    // has any elements matching the remaining.
                    if (!_subIterator) {
                        _subIterator = std::make_unique<boost::optional<BSONElementSubIterator>>();
                    }
                    _subIterator->emplace(_arrayIterationState._current.Obj(),
                                          _arrayIterationState.restOfPath.substr(
                                              _arrayIterationState.nextPieceOfPath.size() + 1),
                                          _path->leafArrayBehavior());

                    _subIterator->value().cursor._arrayIterationState.reset(
                        _subIterator->value().path.fieldRef(), 0);
                    _subIterator->value().cursor._arrayIterationState.startIterator(eltInArray);
                    _subIterator->value().cursor._state = IN_ARRAY;

                    // Set _arrayIterationState._current to EOO. This is not an implicit array
                    // traversal, so we should not override the array offset of the subcursor with
                    // the current array offset.
                    _arrayIterationState._current = BSONElement();

                    if (subCursorHasMore()) {
                        return true;
                    }
                }
            }
        }

        if (_arrayIterationState.hasMore) {
            return false;
        }

        // All array elements have been iterated over. If the leaf array behavior is to omit the
        // array from the results, then we indicate that there are no more items.
        if (_path->leafArrayBehavior() == ElementPath::LeafArrayBehavior::kTraverseOmitArray) {
            return false;
        }

        // Leaf array behavior is kTraverse, therefore return the array we iterated over as the last
        // item.
        _next.reset(_arrayIterationState._theArray, BSONElement());
        _state = DONE;
        return true;
    }

    return false;
}

ElementIterator::Context BSONElementIterator::next() {
    if (_subIterator && _subIterator->has_value()) {
        Context e = _subIterator->value().cursor.next();
        // Use our array offset if we have one, otherwise copy our subcursor's.  This has the
        // effect of preferring the outermost array offset, in the case where we are implicitly
        // traversing nested arrays and have multiple candidate array offsets.  For example,
        // when we use the path "a.b" to generate elements from the document {a: [{b: [1, 2]}]},
        // the element with a value of 2 should be returned with an array offset of 0.
        if (!_arrayIterationState._current.eoo()) {
            e.setArrayOffset(_arrayIterationState._current);
        }
        return e;
    }
    Context x = _next;
    _next.reset();
    return x;
}

BSONElementSubIterator::BSONElementSubIterator(
    const BSONObj& objectToIterate,
    StringData pathToIterate,
    ElementPath::LeafArrayBehavior leafArrayBehavior,
    ElementPath::NonLeafArrayBehavior nonLeafArrayBehavior)
    : path(pathToIterate, leafArrayBehavior, nonLeafArrayBehavior),
      cursor(&path, objectToIterate) {}
}  // namespace mongo
