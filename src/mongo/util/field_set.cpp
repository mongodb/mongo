/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/util/field_set.h"

#include <sstream>

namespace mongo {

namespace {
template <typename FieldListT>
inline void constructFieldSetImpl(FieldListT&& inList,
                                  std::vector<std::string>& outList,
                                  StringSet& outSet) {
    bool hasDuplicates = false;
    auto scanIt = inList.begin();
    auto scanEndIt = inList.end();
    // Scan 'inList' and build 'outSet'.
    for (; scanIt != scanEndIt; ++scanIt) {
        if (MONGO_unlikely(!outSet.insert(*scanIt).second)) {
            hasDuplicates = true;
            break;
        }
    }
    // If there were no duplicates, we can simply copy/move 'inList' into 'outList' and return.
    if (!hasDuplicates) {
        outList = std::forward<FieldListT>(inList);
        return;
    }
    // If there were duplicates, we take the slow path. We start by creating either 3 normal
    // iterator variables or 3 move iterator variables (depending on whether 'inList' was passed
    // by const reference).
    auto makeIter = [](auto&& it) {
        if constexpr (!std::is_const_v<std::remove_reference_t<FieldListT>>) {
            return std::make_move_iterator(it);
        } else {
            return it;
        }
    };
    auto it = makeIter(inList.begin());
    auto midIt = makeIter(scanIt);
    auto endIt = makeIter(scanEndIt);
    // 'midIt' points to the first duplicate we encountered where the initial scan stopped.
    // Take all the values from the range [it, midIt) and copy/move them to 'outList'.
    for (; it != midIt; ++it) {
        outList.emplace_back(*it);
    }
    // Pick up where the initial scan left off and finish building 'outSet' and 'outList'
    // and then return.
    for (; it != endIt; ++it) {
        if (outSet.insert(*it).second) {
            outList.emplace_back(*it);
        }
    }
}
}  // namespace

FieldSet::FieldSet(const std::vector<std::string>& fieldList, FieldListScope scope)
    : _scope(scope) {
    constructFieldSetImpl(fieldList, _list, _set);
}

FieldSet::FieldSet(std::vector<std::string>&& fieldList, FieldListScope scope) : _scope(scope) {
    constructFieldSetImpl(std::move(fieldList), _list, _set);
}

namespace {
void unionInPlaceImpl(std::vector<std::string>& lhsList,
                      StringSet& lhsSet,
                      const std::vector<std::string>& rhs) {
    for (auto&& str : rhs) {
        auto [_, inserted] = lhsSet.emplace(str);
        if (inserted) {
            lhsList.emplace_back(str);
        }
    }
}

void intersectOrDiffInPlaceImpl(std::vector<std::string>& lhsList,
                                StringSet& lhsSet,
                                const StringSet& rhs,
                                bool doIntersect) {
    size_t outIdx = 0;
    for (size_t idx = 0; idx < lhsList.size(); ++idx) {
        bool found = rhs.count(lhsList[idx]);
        if (found == doIntersect) {
            if (outIdx != idx) {
                lhsList[outIdx] = std::move(lhsList[idx]);
            }
            ++outIdx;
        } else {
            lhsSet.erase(lhsList[idx]);
        }
    }

    if (outIdx != lhsList.size()) {
        lhsList.resize(outIdx);
    }
}

void intersectInPlaceImpl(std::vector<std::string>& lhsList,
                          StringSet& lhsSet,
                          const StringSet& rhs) {
    constexpr bool doIntersect = true;
    intersectOrDiffInPlaceImpl(lhsList, lhsSet, rhs, doIntersect);
}

void differenceInPlaceImpl(std::vector<std::string>& lhsList,
                           StringSet& lhsSet,
                           const StringSet& rhs) {
    constexpr bool doIntersect = false;
    intersectOrDiffInPlaceImpl(lhsList, lhsSet, rhs, doIntersect);
}

std::vector<std::string> differenceImpl(const std::vector<std::string>& lhs, const StringSet& rhs) {
    std::vector<std::string> result;
    for (auto&& str : lhs) {
        if (!rhs.count(str)) {
            result.emplace_back(str);
        }
    }
    return result;
}
}  // namespace

void FieldSet::setUnionOrIntersectImpl(const FieldSet& other, bool doUnion) {
    bool isClosed = _scope == FieldListScope::kClosed;
    bool otherIsClosed = other._scope == FieldListScope::kClosed;

    if (isClosed == doUnion) {
        if (otherIsClosed == doUnion) {
            unionInPlaceImpl(_list, _set, other._list);
        } else {
            _list = differenceImpl(other._list, _set);
            _set = StringSet(_list.begin(), _list.end());
            _scope = doUnion ? FieldListScope::kOpen : FieldListScope::kClosed;
        }
    } else {
        if (otherIsClosed == doUnion) {
            differenceInPlaceImpl(_list, _set, other._set);
        } else {
            intersectInPlaceImpl(_list, _set, other._set);
        }
    }
}

std::string FieldSet::toString() const {
    std::stringstream ss;
    ss << (_scope == FieldListScope::kClosed ? "Closed, {" : "Open, {");

    bool first = true;
    for (auto&& field : _list) {
        if (!first) {
            ss << ", ";
        }
        first = false;

        ss << field;
    }

    ss << "}";

    return ss.str();
}

}  // namespace mongo
