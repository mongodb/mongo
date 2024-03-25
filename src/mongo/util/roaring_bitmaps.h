/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

// Prevent macro redefinition warning.
#ifdef IS_BIG_ENDIAN
#undef IS_BIG_ENDIAN
#endif
#include <roaring.hh>
#undef IS_BIG_ENDIAN

#include <absl/container/btree_map.h>

namespace mongo {
/**
 * Roarinng Bitmaps implementation for 64 bit integers. It uses B-Tree map to store 32-bits roaring
 * bitmaps for memory efficiency.
 */
class Roaring64BTree {
public:
    /**
     * Add the value to the bitmaps. Returns true if a new values was added, false otherwise.
     */
    bool addChecked(uint64_t value) {
        return get(highBytes(value)).addChecked(lowBytes(value));
    }

    /**
     * Add the value to the bitmaps.
     */
    void add(uint64_t value) {
        get(highBytes(value)).add(lowBytes(value));
    }

    /**
     * Return true if the value is in the set, false otherwise.
     */
    bool contains(uint64_t value) const {
        return _roarings.contains(highBytes(value))
            ? _roarings.at(highBytes(value)).contains(lowBytes(value))
            : false;
    }

private:
    static constexpr uint32_t highBytes(const uint64_t in) {
        return uint32_t(in >> 32);
    }

    static constexpr uint32_t lowBytes(const uint64_t in) {
        return uint32_t(in);
    }

    roaring::Roaring& get(uint32_t key) {
        auto& roaring = _roarings[key];
        roaring.setCopyOnWrite(false);
        return roaring;
    }

    absl::btree_map<uint32_t, roaring::Roaring> _roarings;
};

}  // namespace mongo
