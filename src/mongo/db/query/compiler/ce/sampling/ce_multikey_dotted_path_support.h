// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo::ce {
/**
 * If there are arrays at or along the given path,
 * unwinds them and returns the resulting elements one by one.
 * Returns undefined for leaf empty arrays, null for non-leaf
 * empty arrays.
 * This essentially emulates the behaviour of multikey indices.
 */
class MultiKeyDottedPathIterator {
public:
    MultiKeyDottedPathIterator(const std::string& path);
    // Changes the object the iterator is working with.
    // Returns the first BSONElement the btree key generator
    // would use in the keystring for this field (referred to
    // later as a "multikey element").
    // Note that because of the way the btree key generator
    // works, the first multikey element always exists.
    // The next call to getNext/hasNext starts from scratch.
    // [obj] must stay valid throughout its iteration.
    MONGO_COMPILER_ALWAYS_INLINE BSONElement resetObj(const BSONObj* obj) {
        _obj = obj;
        _stack.clear();
        return _getFirstElementRooted(0, *_obj);
    }
    // Returns the next multikey element for this field.
    // Do not call this past the end of the iteration.
    BSONElement getNext();
    // Returns if there are further multikey elements to
    // iterate.
    MONGO_COMPILER_ALWAYS_INLINE bool hasNext() const {
        return _stack.size() > 0;
    }

private:
    // Given an array, extracts the first multikey element from it
    // by unwinding it.
    // [rootIdx] is the index of the current path component.
    BSONElement _getFirstElementRootedArray(size_t rootIdx, BSONElement arr);
    // Given a BSON element, extracts the first multikey element from it.
    // If the given element is an array, unwinds it.
    MONGO_COMPILER_ALWAYS_INLINE BSONElement _getFirstElementRooted(size_t idx, BSONElement elem) {
        for (; idx < _components.size(); idx++) {
            if (elem.type() != BSONType::array && elem.type() != BSONType::object) {
                return nullElt;
            }

            const BSONObj obj = elem.embeddedObject();
            elem = obj[_components[idx]];

            if (elem.type() == BSONType::array) {
                return _getFirstElementRootedArray(idx, elem);
            }
        }

        return elem.eoo() ? nullElt : elem;
    }
    // Given a BSON object, extracts the first multikey element from it.
    // If the given element is an array, unwinds it.
    MONGO_COMPILER_ALWAYS_INLINE BSONElement _getFirstElementRooted(size_t idx,
                                                                    const BSONObj& obj) {
        const BSONElement nestedElem = obj[_components[idx]];

        if (nestedElem.type() == BSONType::array) {
            return _getFirstElementRootedArray(idx, nestedElem);
        }

        return _getFirstElementRooted(idx + 1, nestedElem);
    }

private:
    const BSONObj* _obj;
    const std::vector<std::string> _components;
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
