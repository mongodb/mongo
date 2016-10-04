// mongo/util/startup_test.h

/*    Copyright 2009 10gen Inc.
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

#pragma once

#include <vector>

namespace mongo {

/*
   The idea here is to let all initialization of global variables (classes inheriting from
   StartupTest) complete before we run the tests -- otherwise order of initilization being arbitrary
   may mess us up. The app's main() function should call runTests().

   To define a unit test, inherit from this and implement run. instantiate one object for the new
   class as a global.

   These tests are ran on *every* startup of mongod, so they have to be very lightweight.  But it is
   a good quick check for a bad build.
*/
class StartupTest {
public:
    static void runTests();

    static bool testsInProgress() {
        return running;
    }

protected:
    StartupTest();
    virtual ~StartupTest();

private:
    static std::vector<StartupTest*>* tests;
    static bool running;

    static void registerTest(StartupTest* t);

    // assert if fails
    virtual void run() = 0;
};


}  // namespace mongo
