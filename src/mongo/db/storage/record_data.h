// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer.h"

#include <type_traits>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * A replacement for the Record class. This class represents data in a record store.
 * The _ownedData attribute is used to manage memory ownership.
 */
class RecordData {
public:
    RecordData() : _data(nullptr), _size(0) {}
    RecordData(const char* data, int size) : _data(data), _size(size) {}

    RecordData(SharedBuffer ownedData, int size)
        : _data(ownedData.get()), _size(size), _ownedData(std::move(ownedData)) {}

    const char* data() const {
        return _data;
    }

    int size() const {
        return _size;
    }

    /**
     * Returns true if this owns its own memory, and false otherwise
     */
    bool isOwned() const {
        return _size == 0 || _ownedData.get();
    }

    SharedBuffer releaseBuffer() {
        return std::move(_ownedData);
    }

    BSONObj toBson() const& {
        return isOwned() ? BSONObj(_ownedData) : BSONObj(_data);
    }

    BSONObj releaseToBson() {
        return isOwned() ? BSONObj(releaseBuffer()) : BSONObj(_data);
    }

    BSONObj toBson() && {
        return releaseToBson();
    }

    RecordData getOwned() const {
        if (isOwned())
            return *this;
        auto buffer = SharedBuffer::allocate(_size);
        memcpy(buffer.get(), _data, _size);
        return RecordData(buffer, _size);
    }

    void makeOwned() {
        if (isOwned())
            return;
        *this = getOwned();
    }

private:
    const char* _data;
    int _size;
    SharedBuffer _ownedData;
};

MONGO_STATIC_ASSERT(std::is_nothrow_move_constructible_v<RecordData>);
MONGO_STATIC_ASSERT(std::is_nothrow_move_assignable_v<RecordData>);

}  // namespace mongo
