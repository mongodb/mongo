/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * Unit tests of the ActionSet type.
 */

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ActionSetTest, ParseActionSetFromString) {
    ActionSet result;
    ASSERT_OK(ActionSet::parseActionSetFromString("find,insert,update,remove", &result));
    ASSERT_TRUE(result.contains(ActionType::find));
    ASSERT_TRUE(result.contains(ActionType::insert));
    ASSERT_TRUE(result.contains(ActionType::update));
    ASSERT_TRUE(result.contains(ActionType::remove));

    // Order of the strings doesn't matter
    ASSERT_OK(ActionSet::parseActionSetFromString("update,find,remove,insert", &result));
    ASSERT_TRUE(result.contains(ActionType::find));
    ASSERT_TRUE(result.contains(ActionType::insert));
    ASSERT_TRUE(result.contains(ActionType::update));
    ASSERT_TRUE(result.contains(ActionType::remove));

    ASSERT_OK(ActionSet::parseActionSetFromString("find", &result));

    ASSERT_TRUE(result.contains(ActionType::find));
    ASSERT_FALSE(result.contains(ActionType::insert));
    ASSERT_FALSE(result.contains(ActionType::update));
    ASSERT_FALSE(result.contains(ActionType::remove));

    ASSERT_OK(ActionSet::parseActionSetFromString("", &result));

    ASSERT_FALSE(result.contains(ActionType::find));
    ASSERT_FALSE(result.contains(ActionType::insert));
    ASSERT_FALSE(result.contains(ActionType::update));
    ASSERT_FALSE(result.contains(ActionType::remove));

    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  ActionSet::parseActionSetFromString("INVALID INPUT", &result).code());
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
    ASSERT_OK(ActionSet::parseActionSetFromString("find,update,insert", &set1));
    ASSERT_OK(ActionSet::parseActionSetFromString("find,update,remove", &set2));
    ASSERT_OK(ActionSet::parseActionSetFromString("find,update", &set3));

    ASSERT_FALSE(set1.isSupersetOf(set2));
    ASSERT_TRUE(set1.isSupersetOf(set3));

    ASSERT_FALSE(set2.isSupersetOf(set1));
    ASSERT_TRUE(set2.isSupersetOf(set3));

    ASSERT_FALSE(set3.isSupersetOf(set1));
    ASSERT_FALSE(set3.isSupersetOf(set2));
}

TEST(ActionSetTest, anyAction) {
    ActionSet set;

    ASSERT_OK(ActionSet::parseActionSetFromString("anyAction", &set));
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
