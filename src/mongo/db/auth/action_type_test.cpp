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
 * Unit tests of the ActionType type.
 */

#include "mongo/db/auth/action_type.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_OK(EXPR) ASSERT_EQUALS(Status::OK(), (EXPR))

namespace mongo {
namespace {

    TEST(ActionTypeTest, ParseActionFromString) {
        ActionType result;
        ASSERT_OK(ActionType::parseActionFromString("r", &result));
        ASSERT_EQUALS(ActionType::READ.getIdentifier(), result.getIdentifier());
        ASSERT_OK(ActionType::parseActionFromString("w", &result));
        ASSERT_EQUALS(ActionType::READ_WRITE, result);
        ASSERT_OK(ActionType::parseActionFromString("u", &result));
        ASSERT_EQUALS(ActionType::USER_ADMIN, result);
        ASSERT_OK(ActionType::parseActionFromString("d", &result));
        ASSERT_EQUALS(ActionType::DB_ADMIN, result);
        ASSERT_OK(ActionType::parseActionFromString("s", &result));
        ASSERT_EQUALS(ActionType::SERVER_ADMIN, result);
        ASSERT_OK(ActionType::parseActionFromString("c", &result));
        ASSERT_EQUALS(ActionType::CLUSTER_ADMIN, result);

        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      ActionType::parseActionFromString("INVALID INPUT", &result).code());
        ASSERT_EQUALS(ErrorCodes::FailedToParse,
                      ActionType::parseActionFromString("", &result).code());
    }

    TEST(ActionTypeTest, ActionToString) {
        ASSERT_EQUALS("r", ActionType::actionToString(ActionType::READ));
        ASSERT_EQUALS("w", ActionType::actionToString(ActionType::READ_WRITE));
        ASSERT_EQUALS("u", ActionType::actionToString(ActionType::USER_ADMIN));
        ASSERT_EQUALS("d", ActionType::actionToString(ActionType::DB_ADMIN));
        ASSERT_EQUALS("s", ActionType::actionToString(ActionType::SERVER_ADMIN));
        ASSERT_EQUALS("c", ActionType::actionToString(ActionType::CLUSTER_ADMIN));
    }

}  // namespace
}  // namespace mongo
