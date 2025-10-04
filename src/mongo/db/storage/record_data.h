/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/util/shared_buffer.h"

#include <type_traits>

namespace mongo {

// TODO: Does this need to have move support?
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
