// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Unit tests of the ActionSet type.
 */

#include "mongo/db/auth/action_set.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(ActionSetTest, ParseActionSetFromStringVector) {
    const std::vector<std::string_view> actions1 = {"find"sv, "insert"sv, "update"sv, "remove"sv};
    const std::vector<std::string_view> actions2 = {"update"sv, "find"sv, "remove"sv, "insert"sv};
    std::vector<std::string> unrecognized;

    auto set1 = ActionSet::parseFromStringVector(actions1, &unrecognized);
    ASSERT_TRUE(set1.contains(ActionType::find));
    ASSERT_TRUE(set1.contains(ActionType::insert));
    ASSERT_TRUE(set1.contains(ActionType::update));
    ASSERT_TRUE(set1.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.empty());

    // Order of the strings doesn't matter
    auto set2 = ActionSet::parseFromStringVector(actions2, &unrecognized);
    ASSERT_TRUE(set2.contains(ActionType::find));
    ASSERT_TRUE(set2.contains(ActionType::insert));
    ASSERT_TRUE(set2.contains(ActionType::update));
    ASSERT_TRUE(set2.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.empty());

    // Only one ActionType
    auto findSet = ActionSet::parseFromStringVector({"find"}, &unrecognized);
    ASSERT_TRUE(findSet.contains(ActionType::find));
    ASSERT_FALSE(findSet.contains(ActionType::insert));
    ASSERT_FALSE(findSet.contains(ActionType::update));
    ASSERT_FALSE(findSet.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.empty());

    // Empty string as an ActionType
    auto nonEmptyBlankSet = ActionSet::parseFromStringVector({""}, &unrecognized);
    ASSERT_FALSE(nonEmptyBlankSet.contains(ActionType::find));
    ASSERT_FALSE(nonEmptyBlankSet.contains(ActionType::insert));
    ASSERT_FALSE(nonEmptyBlankSet.contains(ActionType::update));
    ASSERT_FALSE(nonEmptyBlankSet.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.size() == 1);
    ASSERT_TRUE(unrecognized.front().empty());
    unrecognized.clear();

    // Unknown ActionType
    auto unknownSet = ActionSet::parseFromStringVector({"INVALID INPUT"}, &unrecognized);
    ASSERT_TRUE(unknownSet.empty());
    ASSERT_EQ(unrecognized.size(), 1UL);
    ASSERT_TRUE(unrecognized.front() == "INVALID INPUT");
}

TEST(ActionSetTest, ToString) {
    ActionSet actionSet;

    ASSERT_EQUALS("", actionSet.toString());
    actionSet.addAction(ActionType::find);
    ASSERT_EQUALS("find", actionSet.toString());
    actionSet.addAction(ActionType::insert);
    ASSERT_EQUALS("find,insert", actionSet.toString());
    actionSet.addAction(ActionType::update);
    ASSERT_EQUALS("find,insert,update", actionSet.toString());
    actionSet.addAction(ActionType::remove);
    ASSERT_EQUALS("find,insert,remove,update", actionSet.toString());

    // Now make sure adding actions in a different order doesn't change anything.
    ActionSet actionSet2;
    ASSERT_EQUALS("", actionSet2.toString());
    actionSet2.addAction(ActionType::insert);
    ASSERT_EQUALS("insert", actionSet2.toString());
    actionSet2.addAction(ActionType::remove);
    ASSERT_EQUALS("insert,remove", actionSet2.toString());
    actionSet2.addAction(ActionType::find);
    ASSERT_EQUALS("find,insert,remove", actionSet2.toString());
    actionSet2.addAction(ActionType::update);
    ASSERT_EQUALS("find,insert,remove,update", actionSet2.toString());
}

TEST(ActionSetTest, IsSupersetOf) {
    ActionSet set1({ActionType::find, ActionType::update, ActionType::insert});
    ActionSet set2({ActionType::find, ActionType::update, ActionType::remove});
    ActionSet set3({ActionType::find, ActionType::update});

    ASSERT_FALSE(set1.isSupersetOf(set2));
    ASSERT_TRUE(set1.isSupersetOf(set3));

    ASSERT_FALSE(set2.isSupersetOf(set1));
    ASSERT_TRUE(set2.isSupersetOf(set3));

    ASSERT_FALSE(set3.isSupersetOf(set1));
    ASSERT_FALSE(set3.isSupersetOf(set2));
}

TEST(ActionSetTest, anyAction) {
    ActionSet set{ActionType::anyAction};
    ASSERT_TRUE(set.contains(ActionType::find));
    ASSERT_TRUE(set.contains(ActionType::insert));
    ASSERT_TRUE(set.contains(ActionType::anyAction));

    set.removeAllActions();
    set.addAllActions();
    ASSERT_TRUE(set.contains(ActionType::find));
    ASSERT_TRUE(set.contains(ActionType::insert));
    ASSERT_TRUE(set.contains(ActionType::anyAction));

    set.removeAllActions();
    set.addAction(ActionType::anyAction);
    ASSERT_TRUE(set.contains(ActionType::find));
    ASSERT_TRUE(set.contains(ActionType::insert));
    ASSERT_TRUE(set.contains(ActionType::anyAction));

    set.removeAction(ActionType::find);
    ASSERT_FALSE(set.contains(ActionType::find));
    ASSERT_TRUE(set.contains(ActionType::insert));
    ASSERT_FALSE(set.contains(ActionType::anyAction));

    set.addAction(ActionType::find);
    ASSERT_TRUE(set.contains(ActionType::find));
    ASSERT_TRUE(set.contains(ActionType::insert));
    ASSERT_FALSE(set.contains(ActionType::anyAction));

    set.addAction(ActionType::anyAction);
    ASSERT_TRUE(set.contains(ActionType::find));
    ASSERT_TRUE(set.contains(ActionType::insert));
    ASSERT_TRUE(set.contains(ActionType::anyAction));

    ASSERT_EQUALS("anyAction", set.toString());

    set.removeAction(ActionType::anyAction);
    ASSERT_TRUE(set.contains(ActionType::find));
    ASSERT_TRUE(set.contains(ActionType::insert));
    ASSERT_FALSE(set.contains(ActionType::anyAction));

    ASSERT_NOT_EQUALS("anyAction", set.toString());
}

TEST(ActionSetTest, constructor) {
    ActionSet set1{};
    ASSERT_TRUE(set1.empty());

    ActionSet set2{ActionType::find};
    ASSERT_EQUALS("find", set2.toString());

    ActionSet set3{ActionType::find, ActionType::insert};
    ASSERT_TRUE(set3.contains(ActionType::find));
    ASSERT_TRUE(set3.contains(ActionType::insert));
}

TEST(ActionSetTest, DuplicateActions) {
    auto fromString = ActionSet::parseFromStringVector({"find"sv, "find"sv, "insert"sv});
    ASSERT_TRUE(fromString.contains(ActionType::find));
    ASSERT_TRUE(fromString.contains(ActionType::insert));

    ActionSet fromEnum({ActionType::find, ActionType::find, ActionType::insert});
    ASSERT_TRUE(fromEnum.contains(ActionType::find));
    ASSERT_TRUE(fromEnum.contains(ActionType::insert));
}

}  // namespace
}  // namespace mongo
