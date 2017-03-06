/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

template <typename T>
class StreamPutter {
public:
    StreamPutter(const ReplSetTagConfig& tagConfig, const T& item)
        : _tagConfig(&tagConfig), _item(&item) {}
    void put(std::ostream& os) const {
        _tagConfig->put(*_item, os);
    }

private:
    const ReplSetTagConfig* _tagConfig;
    const T* _item;
};

template <typename T>
StreamPutter<T> streamput(const ReplSetTagConfig& tagConfig, const T& item) {
    return StreamPutter<T>(tagConfig, item);
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const StreamPutter<T>& putter) {
    putter.put(os);
    return os;
}

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
    void setUp() {
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
