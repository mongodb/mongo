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

#include <algorithm>
#include <iterator>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

TEST(BsonCheck, CheckNothingLegal) {
    ASSERT_OK(bsonCheckOnlyHasFields("", BSONObj(), std::vector<StringData>()));
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  bsonCheckOnlyHasFields("", BSON("a" << 1), std::vector<StringData>()));
}

const char* const legals[] = {"aField", "anotherField", "thirdField"};

TEST(BsonCheck, CheckHasOnlyOnEmptyObject) {
    ASSERT_OK(bsonCheckOnlyHasFields("", BSONObj(), legals));
}

TEST(BsonCheck, CheckHasOnlyLegalFields) {
    ASSERT_OK(bsonCheckOnlyHasFields("",
                                     BSON("aField"
                                          << "value"
                                          << "thirdField" << 1 << "anotherField" << 2),
                                     legals));
    ASSERT_OK(bsonCheckOnlyHasFields("",
                                     BSON("aField"
                                          << "value"
                                          << "thirdField" << 1),
                                     legals));

    ASSERT_EQUALS(ErrorCodes::BadValue,
                  bsonCheckOnlyHasFields("",
                                         BSON("aField"
                                              << "value"
                                              << "illegal" << 4 << "thirdField" << 1),
                                         legals));
}

TEST(BsonCheck, CheckNoDuplicates) {
    ASSERT_EQUALS(51000,
                  bsonCheckOnlyHasFields(
                      "", BSON("aField" << 1 << "anotherField" << 2 << "aField" << 3), legals)
                      .code());
}

}  // namespace
}  // namespace mongo
