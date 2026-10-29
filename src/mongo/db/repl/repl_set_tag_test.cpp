// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_set_tag.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

TEST(ReplSetTagConfigTest, MakeAndFindTags) {
    ReplSetTagConfig tagConfig;
    ReplSetTag dcNY = tagConfig.makeTag("dc", "ny");
    ReplSetTag dcRI = tagConfig.makeTag("dc", "ri");
    ReplSetTag rack1 = tagConfig.makeTag("rack", "1");
    ReplSetTag rack2 = tagConfig.makeTag("rack", "2");
    ASSERT_TRUE(dcNY.isValid());
    ASSERT_EQUALS("dc", tagConfig.getTagKey(dcNY));
    ASSERT_EQUALS("ny", tagConfig.getTagValue(dcNY));
    ASSERT_EQUALS("dc", tagConfig.getTagKey(dcRI));
    ASSERT_EQUALS("ri", tagConfig.getTagValue(dcRI));
    ASSERT_EQUALS("rack", tagConfig.getTagKey(rack1));
    ASSERT_EQUALS("1", tagConfig.getTagValue(rack1));
    ASSERT_EQUALS("rack", tagConfig.getTagKey(rack2));
    ASSERT_EQUALS("2", tagConfig.getTagValue(rack2));

    ASSERT_EQUALS(rack1.getKeyIndex(), rack2.getKeyIndex());
    ASSERT_NOT_EQUALS(rack1.getKeyIndex(), dcRI.getKeyIndex());
    ASSERT_NOT_EQUALS(rack1.getValueIndex(), rack2.getValueIndex());

    ASSERT_TRUE(rack1 == tagConfig.makeTag("rack", "1"));
    ASSERT_TRUE(rack1 == tagConfig.findTag("rack", "1"));
    ASSERT_FALSE(tagConfig.findTag("rack", "7").isValid());
    ASSERT_FALSE(tagConfig.findTag("country", "us").isValid());
}

class ReplSetTagMatchTest : public unittest::Test {
public:
    void setUp() override {
        dcNY = tagConfig.makeTag("dc", "ny");
        dcVA = tagConfig.makeTag("dc", "va");
        dcRI = tagConfig.makeTag("dc", "ri");
        rack1 = tagConfig.makeTag("rack", "1");
        rack2 = tagConfig.makeTag("rack", "2");
        rack3 = tagConfig.makeTag("rack", "3");
        rack4 = tagConfig.makeTag("rack", "4");
    }

protected:
    ReplSetTagConfig tagConfig;
    ReplSetTag dcNY;
    ReplSetTag dcVA;
    ReplSetTag dcRI;
    ReplSetTag rack1;
    ReplSetTag rack2;
    ReplSetTag rack3;
    ReplSetTag rack4;
};

TEST_F(ReplSetTagMatchTest, EmptyPatternAlwaysSatisfied) {
    ReplSetTagPattern pattern = tagConfig.makePattern();
    ASSERT_TRUE(ReplSetTagMatch(pattern).isSatisfied());
    ASSERT_OK(tagConfig.addTagCountConstraintToPattern(&pattern, "dc", 0));
    ASSERT_TRUE(ReplSetTagMatch(pattern).isSatisfied());
}

TEST_F(ReplSetTagMatchTest, SingleTagConstraint) {
    ReplSetTagPattern pattern = tagConfig.makePattern();
    ASSERT_OK(tagConfig.addTagCountConstraintToPattern(&pattern, "dc", 2));
    ReplSetTagMatch matcher(pattern);
    ASSERT_FALSE(matcher.isSatisfied());
    ASSERT_FALSE(matcher.update(dcVA));   // One DC alone won't satisfy "dc: 2".
    ASSERT_FALSE(matcher.update(rack2));  // Adding one rack won't satisfy.
    ASSERT_FALSE(matcher.update(rack3));  // Two racks won't satisfy "dc: 2".
    ASSERT_FALSE(matcher.update(dcVA));   // Same tag twice won't satisfy.
    ASSERT_TRUE(matcher.update(dcRI));    // Two DCs satisfies.
    ASSERT_TRUE(matcher.isSatisfied());
    ASSERT_TRUE(matcher.update(dcNY));   // Three DCs satisfies.
    ASSERT_TRUE(matcher.update(rack1));  // Once matcher is satisfied, it stays satisfied.
}

TEST_F(ReplSetTagMatchTest, MaskingConstraints) {
    // The highest count constraint for a tag key is the only one that matters.
    ReplSetTagPattern pattern = tagConfig.makePattern();
    ASSERT_OK(tagConfig.addTagCountConstraintToPattern(&pattern, "rack", 2));
    ASSERT_OK(tagConfig.addTagCountConstraintToPattern(&pattern, "rack", 3));
    ReplSetTagMatch matcher(pattern);
    ASSERT_FALSE(matcher.isSatisfied());
    ASSERT_FALSE(matcher.update(rack2));
    ASSERT_FALSE(matcher.update(rack3));
    ASSERT_FALSE(matcher.update(rack2));
    ASSERT_TRUE(matcher.update(rack1));
}

TEST_F(ReplSetTagMatchTest, MultipleConstraints) {
    ReplSetTagPattern pattern = tagConfig.makePattern();
    ASSERT_OK(tagConfig.addTagCountConstraintToPattern(&pattern, "dc", 3));
    ASSERT_OK(tagConfig.addTagCountConstraintToPattern(&pattern, "rack", 2));
    ReplSetTagMatch matcher(pattern);
    ASSERT_FALSE(matcher.isSatisfied());
    ASSERT_FALSE(matcher.update(dcVA));
    ASSERT_FALSE(matcher.update(rack2));
    ASSERT_FALSE(matcher.update(rack3));
    ASSERT_FALSE(matcher.update(dcVA));
    ASSERT_FALSE(matcher.update(dcRI));
    ASSERT_TRUE(matcher.update(dcNY));
    ASSERT_TRUE(matcher.isSatisfied());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
