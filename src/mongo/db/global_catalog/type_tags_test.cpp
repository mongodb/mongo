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

#include "mongo/db/global_catalog/type_tags.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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
