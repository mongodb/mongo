// snapshot.h

/**
*    Copyright (C) 2015 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/util/assert_util.h"

namespace mongo {

class SnapshotId {
    static const uint64_t kNullId = 0;

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

private:
    uint64_t _id;
};

template <typename T>
class Snapshotted {
public:
    Snapshotted() : _id(), _value() {}

    Snapshotted(SnapshotId id, const T& value) : _id(id), _value(value) {}

    void reset() {
        *this = Snapshotted();
    }

    void setValue(const T& t) {
        _value = t;
    }

    SnapshotId snapshotId() const {
        return _id;
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
}
