// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Abstraction providing consistent `compare` and `hash_combine` operations for
 * use with polymorphic configurable sorting or to configure unordered
 * associative containers. These closely related operations are bundled into
 * this one interface.
 */
class [[MONGO_MOD_OPEN]] StringDataComparator {
public:
    virtual ~StringDataComparator() = default;

    /**
     * Compares two string values, applying a weak order (i.e. allowing ties).
     * Returns:
     *   <0 if `left < right`
     *    0 if `left == right`
     *   >0 if `left > right`
     */
    virtual int compare(std::string_view left, std::string_view right) const = 0;

    /**
     * Hash `str` in a way consistent with this comparator, storing the
     * result in the `seed` in-out parameter. Strings which `compare` equal
     * must have the same effect on all `seed` values.
     */
    virtual void hash_combine(size_t& seed, std::string_view str) const = 0;
};

/** Uses `std::string_view::compare` and Murmur3 hashing. */
class SimpleStringDataComparator final : public StringDataComparator {
public:
    constexpr int compare(std::string_view left, std::string_view right) const override {
        return left.compare(right);
    }

    void hash_combine(size_t& seed, std::string_view stringToHash) const override;
};

/** Singleton instance for use in basic string comparisons. */
inline const SimpleStringDataComparator simpleStringDataComparator{};

}  // namespace mongo
