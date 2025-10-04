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

/**
 * Unit tests of the ActionSet type.
 */

#include "mongo/db/auth/action_set.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

TEST(ActionSetTest, ParseActionSetFromStringVector) {
    const std::vector<StringData> actions1 = {"find"_sd, "insert"_sd, "update"_sd, "remove"_sd};
    const std::vector<StringData> actions2 = {"update"_sd, "find"_sd, "remove"_sd, "insert"_sd};
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
    auto fromString = ActionSet::parseFromStringVector({"find"_sd, "find"_sd, "insert"_sd});
    ASSERT_TRUE(fromString.contains(ActionType::find));
    ASSERT_TRUE(fromString.contains(ActionType::insert));

    ActionSet fromEnum({ActionType::find, ActionType::find, ActionType::insert});
    ASSERT_TRUE(fromEnum.contains(ActionType::find));
    ASSERT_TRUE(fromEnum.contains(ActionType::insert));
}

}  // namespace
}  // namespace mongo
