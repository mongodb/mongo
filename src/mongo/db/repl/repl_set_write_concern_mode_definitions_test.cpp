/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/repl/repl_set_write_concern_mode_definitions.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
TEST(ReplSetWriteConcernModeDefinitions, Default) {
    auto definitions = ReplSetWriteConcernModeDefinitions();
    BSONObjBuilder bob;
    definitions.serializeToBSON("someTag", &bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), BSON("someTag" << BSONObj()));

    ReplSetTagConfig tagConfig;
    auto tagPatternMapStatus = definitions.convertToTagPatternMap(&tagConfig);
    ASSERT_OK(tagPatternMapStatus.getStatus());
    auto tagPatternMap = tagPatternMapStatus.getValue();
    ASSERT(tagPatternMap.empty());
}

TEST(ReplSetWriteConcernModeDefinitions, Empty) {
    auto definitions = ReplSetWriteConcernModeDefinitions::parseFromBSON(
        BSON("someTag" << BSONObj()).firstElement());
    BSONObjBuilder bob;
    definitions.serializeToBSON("someTag", &bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), BSON("someTag" << BSONObj()));

    ReplSetTagConfig tagConfig;
    auto tagPatternMapStatus = definitions.convertToTagPatternMap(&tagConfig);
    ASSERT_OK(tagPatternMapStatus.getStatus());
    auto tagPatternMap = tagPatternMapStatus.getValue();
    ASSERT(tagPatternMap.empty());
}

TEST(ReplSetWriteConcernModeDefinitions, HasCustomModes) {
    const StringData fieldName("modes"_sd);
    auto writeConcernModes = BSON(
        fieldName << BSON("wc1" << BSON("tag1" << 1 << "tag2" << 2) << "wc2" << BSON("tag3" << 3)));
    auto definitions =
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement());
    BSONObjBuilder bob;
    definitions.serializeToBSON(fieldName, &bob);
    auto roundTrip = bob.obj();
    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(roundTrip, writeConcernModes))
        << "Expected " << writeConcernModes << " == " << roundTrip;

    ReplSetTagConfig tagConfig;
    auto tag1 = tagConfig.makeTag("tag1", "id1");
    auto tag2 = tagConfig.makeTag("tag2", "id2");
    auto tag3 = tagConfig.makeTag("tag3", "id3");
    auto tagPatternMapStatus = definitions.convertToTagPatternMap(&tagConfig);
    ASSERT_OK(tagPatternMapStatus.getStatus());
    auto tagPatternMap = tagPatternMapStatus.getValue();
    ASSERT_EQ(2, tagPatternMap.size());
    ASSERT(tagPatternMap.find("wc1") != tagPatternMap.end());
    ASSERT(tagPatternMap.find("wc2") != tagPatternMap.end());
    ASSERT_EQ(2,
              std::distance(tagPatternMap["wc1"].constraintsBegin(),
                            tagPatternMap["wc1"].constraintsEnd()));
    ASSERT(std::find(tagPatternMap["wc1"].constraintsBegin(),
                     tagPatternMap["wc1"].constraintsEnd(),
                     ReplSetTagPattern::TagCountConstraint(tag1.getKeyIndex(), 1)) !=
           tagPatternMap["wc1"].constraintsEnd());
    ASSERT(std::find(tagPatternMap["wc1"].constraintsBegin(),
                     tagPatternMap["wc1"].constraintsEnd(),
                     ReplSetTagPattern::TagCountConstraint(tag2.getKeyIndex(), 2)) !=
           tagPatternMap["wc1"].constraintsEnd());

    ASSERT_EQ(1,
              std::distance(tagPatternMap["wc2"].constraintsBegin(),
                            tagPatternMap["wc2"].constraintsEnd()));
    ASSERT(std::find(tagPatternMap["wc2"].constraintsBegin(),
                     tagPatternMap["wc2"].constraintsEnd(),
                     ReplSetTagPattern::TagCountConstraint(tag3.getKeyIndex(), 3)) !=
           tagPatternMap["wc2"].constraintsEnd());
}

TEST(ReplSetWriteConcernModeDefinitions, TagMissingFromTagConfig) {
    const StringData fieldName("modes"_sd);
    auto writeConcernModes = BSON(
        fieldName << BSON("wc1" << BSON("tag1" << 1 << "tag2" << 2) << "wc2" << BSON("tag3" << 3)));
    auto definitions =
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement());
    BSONObjBuilder bob;
    definitions.serializeToBSON(fieldName, &bob);
    auto roundTrip = bob.obj();
    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(0, comparator.compare(roundTrip, writeConcernModes))
        << "Expected " << writeConcernModes << " == " << roundTrip;

    ReplSetTagConfig tagConfig;
    tagConfig.makeTag("tag1", "id1");
    tagConfig.makeTag("tag3", "id3");

    auto tagPatternMapStatus = definitions.convertToTagPatternMap(&tagConfig);
    ASSERT_EQ(ErrorCodes::NoSuchKey, tagPatternMapStatus.getStatus());
    ASSERT_STRING_CONTAINS(tagPatternMapStatus.getStatus().reason(), "tag2");
}

TEST(ReplSetWriteConcernModeDefinitions, DuplicateModeNames) {
    const StringData fieldName("modes"_sd);
    auto writeConcernModes = BSON(
        fieldName << BSON("wc1" << BSON("tag1" << 1 << "tag2" << 2) << "wc1" << BSON("tag3" << 3)));
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        AssertionException);
}

TEST(ReplSetWriteConcernModeDefinitions, ZeroConstraint) {
    const StringData fieldName("modes"_sd);
    auto writeConcernModes = BSON(
        fieldName << BSON("wc1" << BSON("tag1" << 0 << "tag2" << 2) << "wc2" << BSON("tag3" << 3)));
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        AssertionException);
}

TEST(ReplSetWriteConcernModeDefinitions, NegativeConstraint) {
    const StringData fieldName("modes"_sd);
    auto writeConcernModes = BSON(fieldName << BSON("wc1" << BSON("tag1" << -1 << "tag2" << 2)
                                                          << "wc2" << BSON("tag3" << 3)));
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        AssertionException);
}

TEST(ReplSetWriteConcernModeDefinitions, ModesMustBeObject) {
    auto writeConcernModes = BSON("modes"
                                  << "stringIsBad");
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        ExceptionFor<ErrorCodes::TypeMismatch>);

    writeConcernModes = BSON("modes" << 99);
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        ExceptionFor<ErrorCodes::TypeMismatch>);

    writeConcernModes = BSON("modes" << BSON_ARRAY("a"
                                                   << "b"
                                                   << "c"));
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        ExceptionFor<ErrorCodes::TypeMismatch>);
}

TEST(ReplSetWriteConcernModeDefinitions, ConstraintsMustBeNumbers) {
    auto writeConcernModes = BSON("modes" << BSON("wc1" << BSON("tag1"
                                                                << "1"
                                                                << "tag2" << 2)
                                                        << "wc2" << BSON("tag3" << 3)));
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        ExceptionFor<ErrorCodes::TypeMismatch>);

    writeConcernModes = BSON("modes" << BSON("wc1" << BSON("tag1" << BSONObj() << "tag2" << 2)
                                                   << "wc2" << BSON("tag3" << 3)));
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        ExceptionFor<ErrorCodes::TypeMismatch>);

    writeConcernModes = BSON("modes" << BSON("wc1" << BSON("tag1" << BSON_ARRAY(1) << "tag2" << 2)
                                                   << "wc2" << BSON("tag3" << 3)));
    ASSERT_THROWS(
        ReplSetWriteConcernModeDefinitions::parseFromBSON(writeConcernModes.firstElement()),
        ExceptionFor<ErrorCodes::TypeMismatch>);
}

}  // namespace repl
}  // namespace mongo
