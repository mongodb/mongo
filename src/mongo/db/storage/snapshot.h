// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class SnapshotId {
    static constexpr uint64_t kNullId = 0;

public:
    SnapshotId() : _id(kNullId) {}

    // 0 is NULL
    explicit SnapshotId(uint64_t id) : _id(id) {
        invariant(id != kNullId);
    }

    bool isNull() const {
        return _id == kNullId;
    }

    bool operator==(const SnapshotId& other) const {
        return _id == other._id;
    }

    bool operator!=(const SnapshotId& other) const {
        return _id != other._id;
    }

    std::string toString() const {
        return std::to_string(_id);
    }

    uint64_t toNumber() const {
        return _id;
    }

private:
    uint64_t _id;
};

inline std::ostream& operator<<(std::ostream& stream, const SnapshotId& snapshotId) {
    return stream << "SnapshotId(" << snapshotId.toNumber() << ")";
}

inline StringBuilder& operator<<(StringBuilder& stream, const SnapshotId& snapshotId) {
    return stream << "SnapshotId(" << snapshotId.toNumber() << ")";
}

template <typename T>
class Snapshotted {
public:
    Snapshotted() : _id(), _value() {}

    Snapshotted(SnapshotId id, const T& value) : _id(id), _value(value) {}
    Snapshotted(SnapshotId id, T&& value) : _id(id), _value(std::forward<T>(value)) {}

    void reset() {
        *this = Snapshotted();
    }

    void setValue(const T& t) {
        _value = t;
    }

    SnapshotId snapshotId() const {
        return _id;
    }

    void setSnapshotId(SnapshotId id) {
        _id = id;
    }

    const T& value() const {
        return _value;
    }
    T& value() {
        return _value;
    }

private:
    SnapshotId _id;
    T _value;
};
}  // namespace mongo
