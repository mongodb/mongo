/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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

}  // namespace
}  // namespace mongo
