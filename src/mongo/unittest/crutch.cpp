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

/**
 * This file should go away.  It contains stubs of functions that were needed to link the unit test
 * framework.  As we refactor the system, the contents of this file should _ONLY_ shrink, and
 * eventually it should contain nothing.
 */

#include "mongo/pch.h"

#include "mongo/db/lasterror.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/goodies.h"
#include "mongo/util/startup_test.h"

namespace mongo {
    StartupTest::StartupTest() {}
    StartupTest::~StartupTest() {}
    bool inShutdown() { return false; }
    void setLastError(int code, const char* msg) {}
    bool StaticObserver::_destroyingStatics = false;
}  // namespace mongo
