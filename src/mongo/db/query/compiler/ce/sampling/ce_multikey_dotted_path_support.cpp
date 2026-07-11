// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

MultiKeyDottedPathIterator::MultiKeyDottedPathIterator(const std::string& path)
    : _obj(&nullObj), _components(verifyPath(absl::StrSplit(path, "."))) {
    _stack.reserve(_components.size());
}

BSONElement MultiKeyDottedPathIterator::getNext() {
    auto&& [iterator, pathIndex] = *_stack.rbegin();
    auto elem = iterator.next();

    const bool itMore = iterator.more();

    if (!itMore) {
        _stack.pop_back();
    }

    return _getFirstElementRooted(pathIndex + 1, elem);
}

static bool isNumericPathComponent(const std::string& component) {
    return (component.size() == 1 || component[0] != '0') && str::isAllDigits(component);
}


BSONElement MultiKeyDottedPathIterator::_getFirstElementRootedArray(size_t idx, BSONElement arr) {
    if (idx + 1 < _components.size() && isNumericPathComponent(_components[idx + 1])) {
        const BSONElement nestedElement = arr.embeddedObject()[_components[idx + 1]];
        if (nestedElement.type() != BSONType::array ||
            (idx + 2 == _components.size() &&
             !nestedElement.embeddedObject().firstElement().eoo())) {
            return _getFirstElementRooted(idx + 2, nestedElement);
        }
        arr = nestedElement;
        idx++;
    }
    auto it = BSONObjIterator(arr.embeddedObject());
    if (!it.more()) {
        // Empty array
        // Yield undefined if leaf, null otherwise.
        return (idx == _components.size() - 1) ? undefinedElt : nullElt;
    }
    auto item = it.next();
    if (it.more()) {
        _stack.push_back(std::make_pair(it, idx));
    }

    return _getFirstElementRooted(idx + 1, item);
}

const BSONObj MultiKeyDottedPathIterator::nullObj = BSON("" << BSONNULL);
const BSONElement MultiKeyDottedPathIterator::nullElt = nullObj.firstElement();
const BSONObj MultiKeyDottedPathIterator::undefinedObj = BSON("" << BSONUndefined);
const BSONElement MultiKeyDottedPathIterator::undefinedElt = undefinedObj.firstElement();

}  // namespace mongo::ce
