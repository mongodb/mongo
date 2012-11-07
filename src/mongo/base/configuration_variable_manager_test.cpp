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
 * Unit tests of the ConfigurationVariableManager type.
 */

#include "mongo/base/configuration_variable_manager.h"
#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    TEST(ConfigurationVariableManagerTest, CorrectRegisterAndSet) {
        ConfigurationVariableManager cvars;

        int firstInt = 1;
        int secondInt = 2;
        std::string aString = "Hello";

        ASSERT_OK(cvars.registerVariable("firstInt", &firstInt));
        ASSERT_OK(cvars.registerVariable("secondInt", &secondInt));
        ASSERT_OK(cvars.registerVariable("aString", &aString));

        // Registering doesn't change values.
        ASSERT_EQUALS(1, firstInt);
        ASSERT_EQUALS(2, secondInt);
        ASSERT_EQUALS("Hello", aString);

        ASSERT_OK(cvars.setVariable("firstInt", "7"));
        ASSERT_EQUALS(7, firstInt);
        ASSERT_OK(cvars.setVariable("firstInt", "8"));
        ASSERT_EQUALS(8, firstInt);
        ASSERT_OK(cvars.setVariable("secondInt", "9"));
        ASSERT_OK(cvars.setVariable("aString", "Goodbye"));
    }

    TEST(ConfigurationVariableManager, ParseFancyNumbers) {
        ConfigurationVariableManager cvars;
        int v = 0;
        ASSERT_OK(cvars.registerVariable("v", &v));
        ASSERT_OK(cvars.setVariable("v", "0xf"));
        ASSERT_EQUALS(0xf, v);
        ASSERT_OK(cvars.setVariable("v", "010"));
        ASSERT_EQUALS(010, v);
    }

    TEST(ConfigurationVariableManagerTest, DoubleRegisterFails) {
        ConfigurationVariableManager cvars;
        int a1, a2;
        ASSERT_OK(cvars.registerVariable("a", &a1));
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, cvars.registerVariable("a", &a2));
    }

    TEST(ConfigurationVariableManagerTest, RegisterNullFails) {
        ConfigurationVariableManager cvars;
        ASSERT_EQUALS(ErrorCodes::BadValue, cvars.registerVariable<int>("a", NULL));
    }

    TEST(ConfigurationVariableManagerTest, IncompatibleSetFails) {
        ConfigurationVariableManager cvars;
        unsigned int v = 12;
        ASSERT_OK(cvars.registerVariable("v", &v));
        ASSERT_OK(cvars.setVariable("v", "15"));
        ASSERT_EQUALS(15U, v);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, cvars.setVariable("v", "fifteen"));
        ASSERT_EQUALS(15U, v);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, cvars.setVariable("v", "-15"));
        ASSERT_EQUALS(15U, v);
    }

    TEST(ConfigurationVariableManagerTest, StringsWithSpacesSettable) {
        ConfigurationVariableManager cvars;
        std::string v;
        ASSERT_OK(cvars.registerVariable("v", &v));
        ASSERT_OK(cvars.setVariable("v", "new value"));
        ASSERT_EQUALS("new value", v);
    }

    TEST(ConfigurationVariableManagerTest, SettingUnregisteredVariableFails) {
        ConfigurationVariableManager cvars;
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, cvars.setVariable("v", "15"));
    }

    TEST(ConfigurationVariableManagerTest, ParseBool) {
        ConfigurationVariableManager cvars;
        bool a;
        ASSERT_OK(cvars.registerVariable("a", &a));
        ASSERT_OK(cvars.setVariable("a", "false"));
        ASSERT_FALSE(a);
        ASSERT_OK(cvars.setVariable("a", "true"));
        ASSERT_TRUE(a);
        ASSERT_OK(cvars.setVariable("a", "false"));
        ASSERT_FALSE(a);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, cvars.setVariable("a", "False"));
    }

}  // namespace
}  // namespace mongo
