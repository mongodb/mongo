// mongo/util/startup_test.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#include "mongo/util/startup_test.h"

namespace mongo {
    std::vector<StartupTest*> *StartupTest::tests = 0;
    bool StartupTest::running = false;

    StartupTest::StartupTest() {
        registerTest(this);
    }

    StartupTest::~StartupTest() {}

    void StartupTest::registerTest( StartupTest *t ) {
        if ( tests == 0 )
            tests = new std::vector<StartupTest*>();
        tests->push_back(t);
    }

    void StartupTest::runTests() {
        running = true;
        for ( std::vector<StartupTest*>::const_iterator i = tests->begin();
              i != tests->end(); i++ ) {

            (*i)->run();
        }
        running = false;
    }

}  // namespace mongo
