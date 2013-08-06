// mongo/unittest/unittest_main.cpp

/*    Copyright 2010 10gen Inc.
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

#include <string>
#include <vector>

#include "base/initializer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/exception_filter_win32.h"

int main( int argc, char **argv, char **envp ) {
    ::mongo::setWindowsUnhandledExceptionFilter();
    ::mongo::runGlobalInitializersOrDie(argc, argv, envp);
    return ::mongo::unittest::Suite::run(std::vector<std::string>(), "", 1);
}

void mongo::unittest::onCurrentTestNameChange( const std::string &testName ) {}
