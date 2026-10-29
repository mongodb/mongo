// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/hint_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

TEST(CommandParsers, ParseKeyPatternHint) {
    auto hint = BSON("hint" << BSON("x" << 5));
    ASSERT_BSONOBJ_EQ(parseHint(hint.firstElement()), BSON("x" << 5));
}

TEST(CommandParsers, ParseIndexNameHint) {
    auto hint = BSON("hint" << "x_1");
    ASSERT_BSONOBJ_EQ(parseHint(hint.firstElement()), BSON("$hint" << "x_1"));
}

TEST(CommandParsers, BadHintType) {
    auto hint = BSON("hint" << 1);
    ASSERT_THROWS_CODE(
        parseHint(hint.firstElement()), AssertionException, ErrorCodes::FailedToParse);
}

TEST(AggregationRequestTest, ShouldRejectHintAsArray) {
    BSONObj arrayHint = BSON("hint" << BSON_ARRAY("invalid" << "hint"));
    ASSERT_THROWS_CODE(
        parseHint(arrayHint.firstElement()), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CommandParsers, SerializeNonEmptyHint) {
    auto hint = BSON("x" << 1);
    BSONObjBuilder bob;
    serializeHintToBSON(hint, "hint", &bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), BSON("hint" << BSON("x" << 1)));
}

TEST(CommandParsers, ShouldNotSerializeEmptyHint) {
    BSONObjBuilder bob;
    serializeHintToBSON(BSONObj(), "hint", &bob);
    ASSERT_FALSE(bob.obj().hasField("hint"));
}
}  // namespace

}  // namespace mongo
