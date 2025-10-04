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
        auto [outSetIt, inserted] = outSet.insert(*it);
        if (inserted) {
            outList.emplace_back(*outSetIt);
        }
    }
}

// This function computes either "lhs union rhs" or "rhs - lhs" (depending on 'isUnion') and stores
// the result in 'lhs'.
void unionOrReverseDiffImpl(std::vector<std::string>& lhsList,
                            StringSet& lhsSet,
                            const std::vector<std::string>& rhs,
                            bool isUnion) {
    if (!isUnion) {
        lhsList.clear();
    }
    size_t oldLhsSize = lhsList.size();

    for (auto&& str : rhs) {
        if (!lhsSet.count(str)) {
            lhsList.emplace_back(str);
        }
    }

    if (!isUnion) {
        lhsSet.clear();
    }
    lhsSet.insert(lhsList.begin() + oldLhsSize, lhsList.end());
}

// This function computes either "lhs intersect rhs" or "lhs - rhs" (depending on 'isIntersect')
// and stores the result in 'lhs'.
void intersectOrDiffImpl(std::vector<std::string>& lhsList,
                         StringSet& lhsSet,
                         const StringSet& rhs,
                         bool isIntersect) {
    size_t outIdx = 0;
    for (size_t idx = 0; idx < lhsList.size(); ++idx) {
        bool found = rhs.count(lhsList[idx]);
        if (found == isIntersect) {
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
}  // namespace

FieldSet::FieldSet(const std::vector<std::string>& fieldList, FieldListScope scope)
    : _scope(scope) {
    constructFieldSetImpl(fieldList, _list, _set);
}

FieldSet::FieldSet(std::vector<std::string>&& fieldList, FieldListScope scope) : _scope(scope) {
    constructFieldSetImpl(std::move(fieldList), _list, _set);
}

void FieldSet::unionOrIntersect(const FieldSet& other, bool isUnion, bool complementOther) {
    bool isClosed = _scope == FieldListScope::kClosed;
    bool otherIsClosed = other._scope == FieldListScope::kClosed;

    if (complementOther) {
        otherIsClosed = !otherIsClosed;
    }

    if (isClosed == isUnion) {
        const bool doUnion = isClosed == otherIsClosed;
        unionOrReverseDiffImpl(_list, _set, other._list, doUnion);
    } else {
        const bool doIntersect = isClosed == otherIsClosed;
        intersectOrDiffImpl(_list, _set, other._set, doIntersect);
    }

    bool scopeHasChanged = isUnion ? (isClosed && !otherIsClosed) : (!isClosed && otherIsClosed);
    if (scopeHasChanged) {
        _scope = isClosed ? FieldListScope::kOpen : FieldListScope::kClosed;
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
