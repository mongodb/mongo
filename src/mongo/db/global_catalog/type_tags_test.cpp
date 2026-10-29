// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_tags.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

using std::string;

TEST(TagsType, Valid) {
    BSONObj obj =
        BSON(TagsType::ns("test.mycol") << TagsType::tag("tag") << TagsType::min(BSON("a" << 10))
                                        << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_TRUE(status.isOK());

    TagsType tag = status.getValue();

    ASSERT_EQUALS(tag.getNS().ns_forTest(), "test.mycol");
    ASSERT_EQUALS(tag.getTag(), "tag");
    ASSERT_BSONOBJ_EQ(tag.getMinKey(), BSON("a" << 10));
    ASSERT_BSONOBJ_EQ(tag.getMaxKey(), BSON("a" << 20));
}

TEST(TagsType, MissingNsField) {
    BSONObj obj = BSON(TagsType::tag("tag")
                       << TagsType::min(BSON("a" << 10)) << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

TEST(TagsType, MissingTagField) {
    BSONObj obj = BSON(TagsType::ns("test.mycol")
                       << TagsType::min(BSON("a" << 10)) << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

TEST(TagsType, MissingMinKey) {
    BSONObj obj =
        BSON(TagsType::ns("test.mycol") << TagsType::tag("tag") << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::IDLFailedToParse, status.getStatus());
}

TEST(TagsType, MissingMaxKey) {
    BSONObj obj =
        BSON(TagsType::ns("test.mycol") << TagsType::tag("tag") << TagsType::min(BSON("a" << 10)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::IDLFailedToParse, status.getStatus());
}

TEST(TagsType, KeysWithDifferentNumberOfColumns) {
    BSONObj obj = BSON(TagsType::ns("test.mycol")
                       << TagsType::tag("tag") << TagsType::min(BSON("a" << 10 << "b" << 10))
                       << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    const TagsType& tag = status.getValue();
    ASSERT_EQUALS(ErrorCodes::BadValue, tag.validate());
}

TEST(TagsType, KeysWithDifferentColumns) {
    BSONObj obj =
        BSON(TagsType::ns("test.mycol") << TagsType::tag("tag") << TagsType::min(BSON("a" << 10))
                                        << TagsType::max(BSON("b" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    const TagsType& tag = status.getValue();
    ASSERT_EQUALS(ErrorCodes::BadValue, tag.validate());
}

TEST(TagsType, KeysNotAscending) {
    BSONObj obj =
        BSON(TagsType::tag("tag") << TagsType::ns("test.mycol") << TagsType::min(BSON("a" << 20))
                                  << TagsType::max(BSON("a" << 10)));

    StatusWith<TagsType> tagStatus = TagsType::fromBSON(obj);
    ASSERT_EQUALS(ErrorCodes::BadValue, tagStatus.getStatus());
}

TEST(TagsType, BadType) {
    BSONObj obj = BSON(TagsType::tag() << 0);

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

}  // namespace
