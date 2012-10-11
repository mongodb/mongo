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

#define ASSERT_OK(EXPR) ASSERT_EQUALS(Status::OK(), (EXPR))

namespace mongo {
namespace {

    TEST(ActionSetTest, ParseActionSetFromString) {
        ActionSet result;
        ASSERT_OK(ActionSet::parseActionSetFromString("r,w,u,d,s,c", &result));
        ASSERT_TRUE(result.contains(ActionType::READ));
        ASSERT_TRUE(result.contains(ActionType::READ_WRITE));
        ASSERT_TRUE(result.contains(ActionType::USER_ADMIN));
        ASSERT_TRUE(result.contains(ActionType::DB_ADMIN));
        ASSERT_TRUE(result.contains(ActionType::SERVER_ADMIN));
        ASSERT_TRUE(result.contains(ActionType::CLUSTER_ADMIN));

        // Order of the letters doesn't matter
        ASSERT_OK(ActionSet::parseActionSetFromString("c,u,s,w,d,r", &result));
        ASSERT_TRUE(result.contains(ActionType::READ));
        ASSERT_TRUE(result.contains(ActionType::READ_WRITE));
        ASSERT_TRUE(result.contains(ActionType::USER_ADMIN));
        ASSERT_TRUE(result.contains(ActionType::DB_ADMIN));
        ASSERT_TRUE(result.contains(ActionType::SERVER_ADMIN));
        ASSERT_TRUE(result.contains(ActionType::CLUSTER_ADMIN));

        ASSERT_OK(ActionSet::parseActionSetFromString("r", &result));

        ASSERT_TRUE(result.contains(ActionType::READ));
        ASSERT_FALSE(result.contains(ActionType::READ_WRITE));
        ASSERT_FALSE(result.contains(ActionType::USER_ADMIN));
        ASSERT_FALSE(result.contains(ActionType::DB_ADMIN));
        ASSERT_FALSE(result.contains(ActionType::SERVER_ADMIN));
        ASSERT_FALSE(result.contains(ActionType::CLUSTER_ADMIN));

        ASSERT_OK(ActionSet::parseActionSetFromString("", &result));

        ASSERT_FALSE(result.contains(ActionType::READ));
        ASSERT_FALSE(result.contains(ActionType::READ_WRITE));
        ASSERT_FALSE(result.contains(ActionType::USER_ADMIN));
        ASSERT_FALSE(result.contains(ActionType::DB_ADMIN));
        ASSERT_FALSE(result.contains(ActionType::SERVER_ADMIN));
        ASSERT_FALSE(result.contains(ActionType::CLUSTER_ADMIN));

        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      ActionSet::parseActionSetFromString("INVALID INPUT", &result).code());
    }

    TEST(ActionSetTest, ToString) {
        ActionSet actionSet;

        ASSERT_EQUALS("", actionSet.toString());
        actionSet.addAction(ActionType::READ);
        ASSERT_EQUALS("r", actionSet.toString());
        actionSet.addAction(ActionType::READ_WRITE);
        ASSERT_EQUALS("r,w", actionSet.toString());
        actionSet.addAction(ActionType::USER_ADMIN);
        ASSERT_EQUALS("r,w,u", actionSet.toString());
        actionSet.addAction(ActionType::DB_ADMIN);
        ASSERT_EQUALS("r,w,u,d", actionSet.toString());
        actionSet.addAction(ActionType::SERVER_ADMIN);
        ASSERT_EQUALS("r,w,u,d,s", actionSet.toString());
        actionSet.addAction(ActionType::CLUSTER_ADMIN);
        ASSERT_EQUALS("r,w,u,d,s,c", actionSet.toString());

        // Now make sure adding actions in a different order doesn't change anything.
        ActionSet actionSet2;
        actionSet2.addAction(ActionType::DB_ADMIN);
        ASSERT_EQUALS("d", actionSet2.toString());
        actionSet2.addAction(ActionType::READ);
        ASSERT_EQUALS("r,d", actionSet2.toString());
        actionSet2.addAction(ActionType::CLUSTER_ADMIN);
        ASSERT_EQUALS("r,d,c", actionSet2.toString());
        actionSet2.addAction(ActionType::USER_ADMIN);
        ASSERT_EQUALS("r,u,d,c", actionSet2.toString());
        actionSet2.addAction(ActionType::SERVER_ADMIN);
        ASSERT_EQUALS("r,u,d,s,c", actionSet2.toString());
        actionSet2.addAction(ActionType::READ_WRITE);
        ASSERT_EQUALS("r,w,u,d,s,c", actionSet2.toString());
    }

    TEST(ActionSetTest, IsSupersetOf) {
        ActionSet set1, set2, set3;
        ASSERT_OK(ActionSet::parseActionSetFromString("r,w,u", &set1));
        ASSERT_OK(ActionSet::parseActionSetFromString("r,w,d", &set2));
        ASSERT_OK(ActionSet::parseActionSetFromString("r,w", &set3));

        ASSERT_FALSE(set1.isSupersetOf(set2));
        ASSERT_TRUE(set1.isSupersetOf(set3));

        ASSERT_FALSE(set2.isSupersetOf(set1));
        ASSERT_TRUE(set2.isSupersetOf(set3));

        ASSERT_FALSE(set3.isSupersetOf(set1));
        ASSERT_FALSE(set3.isSupersetOf(set2));
    }

}  // namespace
}  // namespace mongo
