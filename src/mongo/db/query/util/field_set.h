// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <string>
#include <string_view>
#include <vector>

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

    FieldSet(const std::vector<std::string>& fieldList, FieldListScope scope);

    FieldSet(std::vector<std::string>&& fieldList, FieldListScope scope);

    template <typename ListT>
    FieldSet(const ListT& fieldList, FieldListScope scope) : _scope(scope) {
        for (const auto& field : fieldList) {
            if (_set.insert(field).second) {
                _list.emplace_back(field);
            }
        }
    }

    inline bool count(std::string_view field) const {
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

    bool isEmptySet() const {
        return _list.empty() && _scope == FieldListScope::kClosed;
    }
    bool isUniverseSet() const {
        return _list.empty() && _scope == FieldListScope::kOpen;
    }

    inline void setUnion(const FieldSet& other) {
        constexpr bool doUnion = true;
        constexpr bool complementOther = false;
        unionOrIntersect(other, doUnion, complementOther);
    }
    inline void setUnionWithComplementOf(const FieldSet& other) {
        constexpr bool doUnion = true;
        constexpr bool complementOther = true;
        unionOrIntersect(other, doUnion, complementOther);
    }
    inline void setIntersect(const FieldSet& other) {
        constexpr bool doUnion = false;
        constexpr bool complementOther = false;
        unionOrIntersect(other, doUnion, complementOther);
    }
    inline void setDifference(const FieldSet& other) {
        constexpr bool doUnion = false;
        constexpr bool complementOther = true;
        unionOrIntersect(other, doUnion, complementOther);
    }
    inline void setComplement() {
        bool scopeIsClosed = _scope == FieldListScope::kClosed;
        _scope = scopeIsClosed ? FieldListScope::kOpen : FieldListScope::kClosed;
    }

    std::string toString() const;

private:
    /**
     * Given LHS ('*this') and RHS ('other'), this method computes one of the following
     * expressions (depending on 'isUnion' and 'complementOther') and stores the result
     * into 'this':
     *   LHS intersect RHS (if isUnion == false and complementOther == false)
     *   LHS intersect ~RHS (if isUnion == false and complementOther == true)
     *   LHS union RHS (if isUnion == true and complementOther == false)
     *   LHS union ~RHS (if isUnion == true and complementOther == true)
     */
    void unionOrIntersect(const FieldSet& other, bool isUnion, bool complementOther);

    std::vector<std::string> _list;
    StringSet _set;
    FieldListScope _scope{FieldListScope::kClosed};
};
}  // namespace mongo
