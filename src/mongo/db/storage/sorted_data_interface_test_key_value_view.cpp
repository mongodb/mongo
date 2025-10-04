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

#include "mongo/db/record_id.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

key_string::Value makeKeyString(key_string::Version version,
                                Ordering ordering,
                                BSONObj bsonKey,
                                RecordId& rid) {
    key_string::Builder builder(version, bsonKey, ordering, rid);
    return builder.getValueCopy();
}

TEST(SortedDataInterfaceKeyValueViewTest, SortedDataKeyValueViewTest) {
    BSONObj key = BSON("a" << 1 << "b" << 2.0);
    const Ordering ALL_ASCENDING = Ordering::make(BSONObj());

    char ridBuf[12];
    memset(ridBuf, 0x55, 12);
    RecordId rid(ridBuf);

    for (auto version : {key_string::Version::V0, key_string::Version::V1}) {
        auto keyString = makeKeyString(version, ALL_ASCENDING, key, rid);
        auto view = SortedDataKeyValueView(keyString.getView(),
                                           keyString.getRecordIdView(),
                                           keyString.getTypeBitsView(),
                                           version,
                                           true,
                                           &rid);
        auto value = view.getValueCopy();
        auto bsonObj = key_string::toBson(value, ALL_ASCENDING);
        ASSERT_BSONOBJ_EQ(bsonObj, BSONObj::stripFieldNames(key));
        ASSERT_EQ(&rid, view.getRecordId());
        ASSERT_EQ(*view.getRecordId(), key_string::decodeRecordIdStrAtEnd(value.getView()));

        // Round trip the Value through a View and compare
        auto view2 = SortedDataKeyValueView::fromValue(value);
        ASSERT_EQ(view.getKeyStringOriginalView().size(), view2.getKeyStringOriginalView().size());
        ASSERT_EQ(view.getKeyStringWithoutRecordIdView().size(),
                  view2.getKeyStringWithoutRecordIdView().size());
        ASSERT_EQ(view.getTypeBitsView().size(), view2.getTypeBitsView().size());
        ASSERT_EQ(view.getRecordIdView().size(), view2.getRecordIdView().size());

        auto value2 = view2.getValueCopy();
        ASSERT_EQ(value.compare(value2), 0);
        ASSERT_EQ(view.getKeyStringOriginalView().size(), value2.getSize());
        ASSERT_EQ(view.getKeyStringWithoutRecordIdView().size(), value2.getSizeWithoutRecordId());
        ASSERT_EQ(view.getRecordIdView().size(), value2.getRecordIdSize());
    }
}

}  // namespace
}  // namespace mongo
