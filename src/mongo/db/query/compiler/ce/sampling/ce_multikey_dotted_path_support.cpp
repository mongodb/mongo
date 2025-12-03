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

#include "mongo/db/query/compiler/ce/sampling/ce_multikey_dotted_path_support.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <absl/strings/str_split.h>

namespace mongo::ce {

static std::vector<std::string> verifyPath(std::vector<std::string> path) {
    tassert(109478, "Must have a non-empty path", path.size() > 0);

    for (auto&& component : path) {
        tassert(2308578, "Cannot contain empty field", component.size() > 0);
    }

    return path;
}

MultiKeyDottedPathIterator::MultiKeyDottedPathIterator(const BSONObj* obj, const std::string& path)
    : _obj(obj), _components(verifyPath(absl::StrSplit(path, "."))) {
    _stack.reserve(_components.size());
}

namespace {
const BSONObj nullObj = BSON("" << BSONNULL);
const BSONElement nullElt = nullObj.firstElement();
const BSONObj undefinedObj = BSON("" << BSONUndefined);
const BSONElement undefinedElt = undefinedObj.firstElement();
constexpr size_t M1ULL = std::numeric_limits<size_t>::max();
}  // namespace

void MultiKeyDottedPathIterator::resetObj(const BSONObj* obj) {
    _obj = obj;
    _initialized = false;
    _stack.clear();
}

MONGO_COMPILER_ALWAYS_INLINE std::pair<BSONElement, bool>
MultiKeyDottedPathIterator::nextElement() {
    if (!_initialized) {
        // Accessing the very first element
        _initialized = true;
        return _getFirstElementRooted(0, *_obj);
    } else {
        return _nextElementResume();
    }
}

std::pair<BSONElement, bool> MultiKeyDottedPathIterator::_nextElementResume() {
    auto&& [iterator, pathIndex] = *_stack.rbegin();
    auto elem = iterator.next();

    const bool itMore = iterator.more();

    if (!itMore) {
        _stack.pop_back();
    }

    return _getFirstElementRooted(pathIndex + 1, elem);
}


std::pair<BSONElement, bool> MultiKeyDottedPathIterator::_getFirstElementRootedArray(
    size_t idx, BSONElement arr) {
    auto it = BSONObjIterator(arr.embeddedObject());
    if (!it.more()) {
        // Empty array
        // Yield undefined if leaf, null otherwise.
        return std::make_pair((idx == _components.size() - 1) ? undefinedElt : nullElt,
                              _stack.size() == 0);
    }
    auto item = it.next();
    if (it.more()) {
        _stack.push_back(std::make_pair(it, idx));
    }

    return _getFirstElementRooted(idx + 1, item);
}

MONGO_COMPILER_ALWAYS_INLINE std::pair<BSONElement, bool>
MultiKeyDottedPathIterator::_getFirstElementRooted(const size_t idx, const BSONObj& obj) {
    const BSONElement nestedElem = obj[_components[idx]];
    if (nestedElem.type() == BSONType::eoo) {
        return std::make_pair(nullElt, _stack.size() == 0);
    }

    if (nestedElem.type() == BSONType::array) {
        return _getFirstElementRootedArray(idx, nestedElem);
    }

    return _getFirstElementRooted(idx + 1, nestedElem);
}

MONGO_COMPILER_ALWAYS_INLINE std::pair<BSONElement, bool>
MultiKeyDottedPathIterator::_getFirstElementRooted(size_t idx, BSONElement elem) {
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

}  // namespace mongo::ce
