// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
