/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cstdint>

namespace mongo {

/**
 * Stores the last committed size and count values for a collection.
 */
struct MONGO_MOD_PUBLIC CollectionSizeCount {
    int64_t size{0};
    int64_t count{0};

    bool operator==(const CollectionSizeCount& other) const {
        return size == other.size && count == other.count;
    }
    CollectionSizeCount operator+(const CollectionSizeCount& other) const {
        return CollectionSizeCount{size + other.size, count + other.count};
    }
    CollectionSizeCount operator-(const CollectionSizeCount& other) const {
        return CollectionSizeCount{size - other.size, count - other.count};
    }

    std::string toString() const {
        return str::stream() << "size: " << size << ", count: " << count;
    }
};

inline std::ostream& operator<<(std::ostream& s, const CollectionSizeCount& collectionSizeCount) {
    return (s << collectionSizeCount.toString());
}

/**
 * Indicates whether a collection had been created or dropped since the last checkpoint.
 */
enum class DDLState { kCreated, kDropped, kNone };

/**
 * Stores the size and count values for a collection along with state indicating whether the
 * collection had been created or dropped.
 */
struct SizeCountDelta {
    CollectionSizeCount sizeCount{0, 0};
    DDLState state{DDLState::kNone};

    std::string toString() const {
        return fmt::format("sizeCount: {}, state: {}",
                           sizeCount.toString(),
                           (state == DDLState::kCreated
                                ? "created"
                                : (state == DDLState::kDropped ? "dropped" : "none")));
    }
};

}  // namespace mongo
