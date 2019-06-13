
/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"


#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/hint_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(CommandParsers, ParseKeyPatternHint) {
    auto hint = BSON("hint" << BSON("x" << 5));
    ASSERT_BSONOBJ_EQ(parseHint(hint.firstElement()), BSON("x" << 5));
}

TEST(CommandParsers, ParseIndexNameHint) {
    auto hint = BSON("hint"
                     << "x_1");
    ASSERT_BSONOBJ_EQ(parseHint(hint.firstElement()),
                      BSON("$hint"
                           << "x_1"));
}

TEST(CommandParsers, BadHintType) {
    auto hint = BSON("hint" << 1);
    ASSERT_THROWS_CODE(
        parseHint(hint.firstElement()), AssertionException, ErrorCodes::FailedToParse);
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