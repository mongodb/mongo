/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_tags.h"

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
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

    ASSERT_EQUALS(tag.getNS(), "test.mycol");
    ASSERT_EQUALS(tag.getTag(), "tag");
    ASSERT_EQUALS(tag.getMinKey(), BSON("a" << 10));
    ASSERT_EQUALS(tag.getMaxKey(), BSON("a" << 20));
}

TEST(TagsType, MissingNsField) {
    BSONObj obj = BSON(TagsType::tag("tag") << TagsType::min(BSON("a" << 10))
                                            << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

TEST(TagsType, MissingTagField) {
    BSONObj obj = BSON(TagsType::ns("test.mycol") << TagsType::min(BSON("a" << 10))
                                                  << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

TEST(TagsType, MissingMinKey) {
    BSONObj obj =
        BSON(TagsType::ns("test.mycol") << TagsType::tag("tag") << TagsType::max(BSON("a" << 20)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

TEST(TagsType, MissingMaxKey) {
    BSONObj obj =
        BSON(TagsType::ns("test.mycol") << TagsType::tag("tag") << TagsType::min(BSON("a" << 10)));

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_FALSE(status.isOK());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

TEST(TagsType, KeysWithDifferentNumberOfColumns) {
    BSONObj obj = BSON(TagsType::ns("test.mycol") << TagsType::tag("tag")
                                                  << TagsType::min(BSON("a" << 10 << "b" << 10))
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

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    const TagsType& tag = status.getValue();
    ASSERT_EQUALS(ErrorCodes::BadValue, tag.validate());
}

TEST(TagsType, BadType) {
    BSONObj obj = BSON(TagsType::tag() << 0);

    StatusWith<TagsType> status = TagsType::fromBSON(obj);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status.getStatus());
}

}  // namespace
