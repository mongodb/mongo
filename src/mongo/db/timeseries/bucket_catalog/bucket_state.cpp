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

#include "mongo/db/timeseries/bucket_catalog/bucket_state.h"

#include "mongo/util/str.h"

namespace mongo::timeseries::bucket_catalog {

BucketState& BucketState::setFlag(BucketStateFlag flag) {
    _state |= static_cast<decltype(_state)>(flag);
    return *this;
}

BucketState& BucketState::unsetFlag(BucketStateFlag flag) {
    _state &= ~static_cast<decltype(_state)>(flag);
    return *this;
}

BucketState& BucketState::reset() {
    _state = 0;
    return *this;
}

bool BucketState::isSet(BucketStateFlag flag) const {
    return _state & static_cast<decltype(_state)>(flag);
}


bool BucketState::isPrepared() const {
    constexpr decltype(_state) mask = static_cast<decltype(_state)>(BucketStateFlag::kPrepared);
    return _state & mask;
}

bool BucketState::conflictsWithReopening() const {
    constexpr decltype(_state) mask =
        static_cast<decltype(_state)>(BucketStateFlag::kPendingCompression) |
        static_cast<decltype(_state)>(BucketStateFlag::kPendingDirectWrite);
    return _state & mask;
}

bool BucketState::conflictsWithInsertion() const {
    constexpr decltype(_state) mask = static_cast<decltype(_state)>(BucketStateFlag::kCleared) |
        static_cast<decltype(_state)>(BucketStateFlag::kPendingCompression) |
        static_cast<decltype(_state)>(BucketStateFlag::kPendingDirectWrite);
    return _state & mask;
}

bool BucketState::operator==(const BucketState& other) const {
    return _state == other._state;
}

std::string BucketState::toString() const {
    str::stream str;
    str << "[";

    bool first = true;
    auto output = [&first, &str](std::string name) {
        if (first) {
            first = false;
        } else {
            str << ", ";
        }
        str << name;
    };

    if (isSet(BucketStateFlag::kPrepared)) {
        output("prepared");
    }

    if (isSet(BucketStateFlag::kCleared)) {
        output("cleared");
    }

    if (isSet(BucketStateFlag::kPendingCompression)) {
        output("pendingCompression");
    }

    if (isSet(BucketStateFlag::kPendingDirectWrite)) {
        output("pendingDirectWrite");
    }

    if (isSet(BucketStateFlag::kUntracked)) {
        output("untracked");
    }

    str << "]";
    return str;
}

}  // namespace mongo::timeseries::bucket_catalog
