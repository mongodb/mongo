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

#include <string>

#include "mongo/base/data_range_cursor.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/wire_message_payload.h"
#include "mongo/unittest/assert.h"

namespace mongo {

namespace {

TEST(DataTypeWireMessagePayload, EmptyBSONObject) {
    const BSONObj empty;
    ConstDataRangeCursor cdrc(empty.objdata(), empty.objsize());

    WireMessagePayload wmp;

    ASSERT_OK(cdrc.readAndAdvanceNoThrow(&wmp));
    ASSERT_BSONOBJ_EQ(empty, wmp.obj);
}

TEST(DataTypeWireMessagePayload, SmallBSONObject) {
    const BSONObj obj = BSON("foo" << BSON_ARRAY(1 << 2 << 3 << 4 << 5) << "bar"
                                   << "baz");
    ConstDataRangeCursor cdrc(obj.objdata(), obj.objsize());

    WireMessagePayload wmp;

    ASSERT_OK(cdrc.readAndAdvanceNoThrow(&wmp));
    ASSERT_BSONOBJ_EQ(obj, wmp.obj);
}

TEST(DataTypeWireMessagePayload, BSONObjectWith1MPayload) {
    const BSONObj obj = BSON("payload" << std::string(1024 * 1024, 'x'));
    ConstDataRangeCursor cdrc(obj.objdata(), obj.objsize());

    WireMessagePayload wmp;

    ASSERT_OK(cdrc.readAndAdvanceNoThrow(&wmp));
    ASSERT_BSONOBJ_EQ(obj, wmp.obj);
}

TEST(DataTypeWireMessagePayload, BSONObjectAtSizeLimits) {
    // Overhead:
    // - 4 bytes for object size
    // - 1 byte for string type tag
    // - 8 bytes for "payload" field name plus trailing \0 byte
    // - 4 bytes for string field length
    // - 1 byte for trailing \0 byte for string value
    // - 1 byte for trailing \0 byte for object
    // ========================================
    // = 19 bytes total overhead
    auto buildBSONObjAtSizeLimit = [](int sizeLimit) {
        constexpr int kOverheadInBytes = 19;

        BSONObjBuilder bob;
        bob.append("payload", std::string(sizeLimit - kOverheadInBytes, 'x'));
        return bob.obj<BSONObj::LargeSizeTrait>();
    };

    // Build objects that are exactly the size as the different internal size limits.
    for (int sizeLimit : {BSONObjMaxUserSize, BSONObjMaxInternalSize, BSONObjMaxWireMessageSize}) {
        BSONObj obj = buildBSONObjAtSizeLimit(sizeLimit);

        ConstDataRangeCursor cdrc(obj.objdata(), obj.objsize());

        WireMessagePayload wmp;

        ASSERT_OK(cdrc.readAndAdvanceNoThrow(&wmp));
        ASSERT_BSONOBJ_EQ(obj, wmp.obj);
        ASSERT_EQ(sizeLimit, wmp.obj.objsize());
    }

    // Build an object that is larger than the size limit for WireMessagePayloads.
    {
        BSONObj obj = buildBSONObjAtSizeLimit(BSONObjMaxWireMessageSize + 1);
        ConstDataRangeCursor cdrc(obj.objdata(), obj.objsize());

        WireMessagePayload wmp;

        ASSERT_EQUALS(ErrorCodes::BSONObjectTooLarge, cdrc.readAndAdvanceNoThrow(&wmp).code());
    }
}

}  // namespace

}  // namespace mongo
