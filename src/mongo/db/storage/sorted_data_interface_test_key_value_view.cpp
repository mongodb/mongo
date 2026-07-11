// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
