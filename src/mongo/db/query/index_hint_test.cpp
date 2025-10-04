/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/index_hint.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

TEST(IndexHint, ParseKeyPatternHint) {
    auto hint = BSON("hint" << BSON("x" << 5));
    ASSERT_BSONOBJ_EQ(*IndexHint::parse(hint.firstElement()).getIndexKeyPattern(), BSON("x" << 5));
}

TEST(IndexHint, ParseIndexNameHint) {
    auto indexName = "x_1";
    auto hint = BSON("hint" << indexName);
    ASSERT_EQ(*IndexHint::parse(hint.firstElement()).getIndexName(), indexName);
}

TEST(IndexHint, ParseNaturalHint) {
    auto hint = BSON("hint" << BSON("$natural" << 1));
    ASSERT_EQ(IndexHint::parse(hint.firstElement()).getNaturalHint()->direction,
              NaturalOrderHint::Direction::kForward);
}

TEST(IndexHint, BadHintType) {
    auto hint = BSON("hint" << 1);
    ASSERT_THROWS_CODE(
        IndexHint::parse(hint.firstElement()), AssertionException, ErrorCodes::FailedToParse);
}

TEST(IndexHint, ShouldRejectHintAsArray) {
    BSONObj arrayHint = BSON("hint" << BSON_ARRAY("invalid" << "hint"));
    ASSERT_THROWS_CODE(
        IndexHint::parse(arrayHint.firstElement()), AssertionException, ErrorCodes::FailedToParse);
}

TEST(IndexHint, ShouldRejectNaturalWithMoreThanOneField) {
    BSONObj arrayHint = BSON("$natural" << 1 << "a" << 1);
    ASSERT_THROWS_CODE(
        IndexHint::parse(arrayHint.firstElement()), AssertionException, ErrorCodes::FailedToParse);
}

TEST(IndexHint, SerializeNonEmptyHint) {
    auto indexKeyPattern = BSON("x" << 1);
    auto hint = IndexHint(indexKeyPattern);
    BSONObjBuilder bob;
    IndexHint::append(hint, "hint", &bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), BSON("hint" << indexKeyPattern));
}

}  // namespace
}  // namespace mongo
