// mongo/util/startup_test.h

/*    Copyright 2009 10gen Inc.
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

#pragma once

#include <vector>

namespace mongo {

    /* The idea here is to let all initialization of global variables (classes inheriting from StartupTest)
       complete before we run the tests -- otherwise order of initilization being arbitrary may mess
       us up.  The app's main() function should call runTests().

       To define a unit test, inherit from this and implement run. instantiate one object for the new class
       as a global.

       These tests are ran on *every* startup of mongod, so they have to be very lightweight.  But it is a
       good quick check for a bad build.
    */
    class StartupTest {
    public:
        static void runTests();

        static bool testsInProgress() { return running; }

    protected:
        StartupTest();
        virtual ~StartupTest();

    private:
        static std::vector<StartupTest*> *tests;
        static bool running;

        static void registerTest(StartupTest *t);

        // assert if fails
        virtual void run() = 0;

    };


} // namespace mongo
