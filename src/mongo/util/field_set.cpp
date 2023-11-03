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
