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

namespace mongo {
namespace {

TEST(ActionSetTest, ParseActionSetFromStringVector) {
    ActionSet result;
    std::vector<std::string> actions1 = {"find", "insert", "update", "remove"};
    std::vector<std::string> actions2 = {"update", "find", "remove", "insert"};
    std::vector<std::string> unrecognized;

    ASSERT_OK(ActionSet::parseActionSetFromStringVector(actions1, &result, &unrecognized));
    ASSERT_TRUE(result.contains(ActionType::find));
    ASSERT_TRUE(result.contains(ActionType::insert));
    ASSERT_TRUE(result.contains(ActionType::update));
    ASSERT_TRUE(result.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.empty());

    // Order of the strings doesn't matter
    ASSERT_OK(ActionSet::parseActionSetFromStringVector(actions2, &result, &unrecognized));
    ASSERT_TRUE(result.contains(ActionType::find));
    ASSERT_TRUE(result.contains(ActionType::insert));
    ASSERT_TRUE(result.contains(ActionType::update));
    ASSERT_TRUE(result.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.empty());

    ASSERT_OK(ActionSet::parseActionSetFromStringVector({"find"}, &result, &unrecognized));

    ASSERT_TRUE(result.contains(ActionType::find));
    ASSERT_FALSE(result.contains(ActionType::insert));
    ASSERT_FALSE(result.contains(ActionType::update));
    ASSERT_FALSE(result.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.empty());

    ASSERT_OK(ActionSet::parseActionSetFromStringVector({""}, &result, &unrecognized));

    ASSERT_FALSE(result.contains(ActionType::find));
    ASSERT_FALSE(result.contains(ActionType::insert));
    ASSERT_FALSE(result.contains(ActionType::update));
    ASSERT_FALSE(result.contains(ActionType::remove));
    ASSERT_TRUE(unrecognized.size() == 1);
    ASSERT_TRUE(unrecognized.front().empty());

    unrecognized.clear();
    ASSERT_OK(ActionSet::parseActionSetFromStringVector({"INVALID INPUT"}, &result, &unrecognized));
    ASSERT_TRUE(unrecognized.size() == 1);
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
    ActionSet set1, set2, set3;
    std::vector<std::string> actions1 = {"find", "update", "insert"};
    std::vector<std::string> actions2 = {"find", "update", "remove"};
    std::vector<std::string> actions3 = {"find", "update"};
    std::vector<std::string> unrecognized;

    ASSERT_OK(ActionSet::parseActionSetFromStringVector(actions1, &set1, &unrecognized));
    ASSERT_OK(ActionSet::parseActionSetFromStringVector(actions2, &set2, &unrecognized));
    ASSERT_OK(ActionSet::parseActionSetFromStringVector(actions3, &set3, &unrecognized));

    ASSERT_FALSE(set1.isSupersetOf(set2));
    ASSERT_TRUE(set1.isSupersetOf(set3));

    ASSERT_FALSE(set2.isSupersetOf(set1));
    ASSERT_TRUE(set2.isSupersetOf(set3));

    ASSERT_FALSE(set3.isSupersetOf(set1));
    ASSERT_FALSE(set3.isSupersetOf(set2));
}

TEST(ActionSetTest, anyAction) {
    ActionSet set;
    std::vector<std::string> actions = {"anyAction"};
    std::vector<std::string> unrecognized;

    ASSERT_OK(ActionSet::parseActionSetFromStringVector(actions, &set, &unrecognized));
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

}  // namespace
}  // namespace mongo
