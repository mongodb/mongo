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
#include "mongo/unittest/unittest.h"

#define ASSERT_OK(EXPR) ASSERT_EQUALS(Status::OK(), (EXPR))

namespace mongo {
namespace {

    TEST(ActionSetTest, ActionStringToAction) {
        ActionSet::ActionType result;
        ASSERT_OK(ActionSet::parseActionFromString("r", &result));
        ASSERT_EQUALS(ActionSet::READ, result);
        ASSERT_OK(ActionSet::parseActionFromString("w", &result));
        ASSERT_EQUALS(ActionSet::WRITE, result);
        ASSERT_OK(ActionSet::parseActionFromString("u", &result));
        ASSERT_EQUALS(ActionSet::USER_ADMIN, result);
        ASSERT_OK(ActionSet::parseActionFromString("p", &result));
        ASSERT_EQUALS(ActionSet::PRODUCTION_ADMIN, result);
        ASSERT_OK(ActionSet::parseActionFromString("a", &result));
        ASSERT_EQUALS(ActionSet::SUPER_ADMIN, result);

        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      ActionSet::parseActionFromString("INVALID INPUT", &result).code());
        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      ActionSet::parseActionFromString("", &result).code());
    }

    TEST(ActionSetTest, ActionsStringToInt) {
        ActionSet result;
        ASSERT_OK(ActionSet::parseActionSetFromString("r,w,u,p,a", &result));
        ASSERT_TRUE(result.contains(ActionSet::READ));
        ASSERT_TRUE(result.contains(ActionSet::WRITE));
        ASSERT_TRUE(result.contains(ActionSet::USER_ADMIN));
        ASSERT_TRUE(result.contains(ActionSet::PRODUCTION_ADMIN));
        ASSERT_TRUE(result.contains(ActionSet::SUPER_ADMIN));

        // Order of the letters doesn't matter
        ASSERT_OK(ActionSet::parseActionSetFromString("a,u,w,p,r", &result));
        ASSERT_TRUE(result.contains(ActionSet::READ));
        ASSERT_TRUE(result.contains(ActionSet::WRITE));
        ASSERT_TRUE(result.contains(ActionSet::USER_ADMIN));
        ASSERT_TRUE(result.contains(ActionSet::PRODUCTION_ADMIN));
        ASSERT_TRUE(result.contains(ActionSet::SUPER_ADMIN));

        ASSERT_OK(ActionSet::parseActionSetFromString("r", &result));

        ASSERT_TRUE(result.contains(ActionSet::READ));
        ASSERT_FALSE(result.contains(ActionSet::WRITE));
        ASSERT_FALSE(result.contains(ActionSet::USER_ADMIN));
        ASSERT_FALSE(result.contains(ActionSet::PRODUCTION_ADMIN));
        ASSERT_FALSE(result.contains(ActionSet::SUPER_ADMIN));

        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      ActionSet::parseActionSetFromString("INVALID INPUT", &result).code());
    }

}  // namespace
}  // namespace mongo
