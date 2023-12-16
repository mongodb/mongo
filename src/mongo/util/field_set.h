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

#pragma once

#include <string>
#include <vector>

#include "mongo/util/string_map.h"

namespace mongo {
/**
 * A "field list" is a list that can be either: (1) a finite list of field names (strings); or
 * (2) the complement of a finite list of field names (where the universe U is the set of all
 * possible field names). In the first case we say the scope of the field list is "closed", and
 * in the second case we say the scope of the field list is "open".
 *
 * The 'FieldListScope' enum is used to represent this concept of "scope" for field lists.
 */
enum class FieldListScope { kClosed, kOpen };

class FieldSet {
public:
    static inline FieldSet makeEmptySet() {
        return FieldSet(std::vector<std::string>{}, FieldListScope::kClosed);
    }
    static inline FieldSet makeUniverseSet() {
        return FieldSet(std::vector<std::string>{}, FieldListScope::kOpen);
    }

    static inline FieldSet makeClosedSet(const std::vector<std::string>& fieldList) {
        return FieldSet(fieldList, FieldListScope::kClosed);
    }
    static inline FieldSet makeClosedSet(std::vector<std::string>&& fieldList) {
        return FieldSet(std::move(fieldList), FieldListScope::kClosed);
    }
    static inline FieldSet makeOpenSet(const std::vector<std::string>& fieldList) {
        return FieldSet(fieldList, FieldListScope::kOpen);
    }
    static inline FieldSet makeOpenSet(std::vector<std::string>&& fieldList) {
        return FieldSet(std::move(fieldList), FieldListScope::kOpen);
    }

    template <typename ListT>
    static inline FieldSet makeClosedSet(const ListT& fieldList) {
        return FieldSet(std::vector<std::string>(fieldList.begin(), fieldList.end()),
                        FieldListScope::kClosed);
    }
    template <typename ListT>
    static inline FieldSet makeOpenSet(const ListT& fieldList) {
        return FieldSet(std::vector<std::string>(fieldList.begin(), fieldList.end()),
                        FieldListScope::kOpen);
    }

    FieldSet() = default;

    FieldSet(const std::vector<std::string>& fieldList, FieldListScope scope)
        : _list(fieldList), _set(_list.begin(), _list.end()), _scope(scope) {}

    FieldSet(std::vector<std::string>&& fieldList, FieldListScope scope)
        : _list(std::move(fieldList)), _set(_list.begin(), _list.end()), _scope(scope) {}

    template <typename ListT>
    FieldSet(const ListT& fieldList, FieldListScope scope)
        : _list(fieldList.begin(), fieldList.end()),
          _set(_list.begin(), _list.end()),
          _scope(scope) {}

    inline bool count(StringData field) const {
        bool scopeIsClosed = _scope == FieldListScope::kClosed;
        bool found = _set.count(field);
        return found == scopeIsClosed;
    }

    inline const std::vector<std::string>& getList() const {
        return _list;
    }
    inline const StringSet& getSet() const {
        return _set;
    }
    inline FieldListScope getScope() const {
        return _scope;
    }

    inline void setUnion(const FieldSet& other) {
        constexpr bool doUnion = true;
        setUnionOrIntersectImpl(other, doUnion);
    }
    inline void setIntersect(const FieldSet& other) {
        constexpr bool doUnion = false;
        setUnionOrIntersectImpl(other, doUnion);
    }
    inline void setComplement() {
        bool scopeIsClosed = _scope == FieldListScope::kClosed;
        _scope = scopeIsClosed ? FieldListScope::kOpen : FieldListScope::kClosed;
    }

    std::string toString() const;

private:
    void setUnionOrIntersectImpl(const FieldSet& other, bool doUnion);

    std::vector<std::string> _list;
    StringSet _set;
    FieldListScope _scope{FieldListScope::kClosed};
};
}  // namespace mongo
