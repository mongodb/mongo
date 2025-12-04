/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo::ce {
/**
 * If there are arrays at or along the given path,
 * unwinds them and returns the resulting elements one by one.
 * Returns undefined for leaf empty arrays, null for non-leaf
 * empty arrays.
 * This essentially emulates the behaviour of multikey indices.
 * [obj] must stay valid throughout its iteration.
 */
class MultiKeyDottedPathIterator {
public:
    MultiKeyDottedPathIterator(const BSONObj* obj, const std::string& path);
    // Changes the object the iterator is working with.
    // The next call to nextElement() starts from scratch.
    void resetObj(const BSONObj* obj);
    // Returns the next BSONElement the btree key generator
    // would use in the keystring for this field (referred to
    // later as a "multikey element").
    // If the second field is true, there are no more
    // elements to iterate over.
    // Note that because of the way the btree key generator
    // works, iterating over an object always returns at least
    // one multikey element.
    // Do not call this past the end of the iteration.
    MONGO_COMPILER_ALWAYS_INLINE std::pair<BSONElement, bool> nextElement() {
        if (!_initialized) {
            // Accessing the very first element
            _initialized = true;
            return _getFirstElementRooted(0, *_obj);
        } else {
            return _nextElementResume();
        }
    }

private:
    // Resumes an existing iteration. Called by nextElement().
    std::pair<BSONElement, bool> _nextElementResume();
    // Given an array, extracts the first multikey element from it
    // by unwinding it.
    // [rootIdx] is the index of the current path component.
    std::pair<BSONElement, bool> _getFirstElementRootedArray(size_t rootIdx, BSONElement arr);
    // Given a BSON element, extracts the first multikey element from it.
    // If the given element is an array, unwinds it.
    MONGO_COMPILER_ALWAYS_INLINE std::pair<BSONElement, bool> _getFirstElementRooted(
        size_t idx, BSONElement elem) {
        for (; idx < _components.size(); idx++) {
            const BSONType elemType = elem.type();
            if (elemType != BSONType::array && elemType != BSONType::object) {
                return std::make_pair(nullElt, _stack.size() == 0);
            }

            const BSONObj obj = elem.embeddedObject();
            const BSONElement nestedElem = obj[_components[idx]];
            if (nestedElem.type() == BSONType::eoo) {
                return std::make_pair(nullElt, _stack.size() == 0);
            }

            if (nestedElem.type() == BSONType::array) {
                return _getFirstElementRootedArray(idx, nestedElem);
            } else {
                elem = nestedElem;
            }
        }

        return std::make_pair(elem, _stack.size() == 0);
    }
    // Given a BSON object, extracts the first multikey element from it.
    // If the given element is an array, unwinds it.
    MONGO_COMPILER_ALWAYS_INLINE std::pair<BSONElement, bool> _getFirstElementRooted(
        size_t idx, const BSONObj& obj) {
        const BSONElement nestedElem = obj[_components[idx]];
        if (nestedElem.type() == BSONType::eoo) {
            return std::make_pair(nullElt, _stack.size() == 0);
        }

        if (nestedElem.type() == BSONType::array) {
            return _getFirstElementRootedArray(idx, nestedElem);
        }

        return _getFirstElementRooted(idx + 1, nestedElem);
    }

private:
    const BSONObj* _obj;
    std::vector<std::string> _components;
    // True if nextElement() has ever been called
    // since the iterator was constructed or the last
    // call to resetObj().
    bool _initialized = false;
    // As we are iterating the object, we need to keep track of
    // the arrays (as we need to unwind them).
    // We do this by pushing (iterator, path index) pairs to a stack.
    // Note that the worst-case size of this is the size of _components,
    // corresponding to the case where every component along the path
    // is an array.
    std::vector<std::pair<BSONObjIterator, size_t>> _stack;

private:
    static const BSONObj nullObj;
    static const BSONElement nullElt;
    static const BSONObj undefinedObj;
    static const BSONElement undefinedElt;
    static constexpr size_t M1ULL = std::numeric_limits<size_t>::max();
};
}  // namespace mongo::ce
