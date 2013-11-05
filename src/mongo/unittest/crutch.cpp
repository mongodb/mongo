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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/client/dbclientinterface.h"
#include "mongo/util/exit_code.h"

namespace mongo {

    bool inShutdown() {
        return false;
    }

    DBClientBase *createDirectClient() {
        fassertFailed(17249);
        return NULL;
    }

    bool haveLocalShardingInfo(const std::string& ns) {
        return false;
    }

    void dbexit(ExitCode rc, const char *why) {
        fassertFailed(17250);
    }

}  // namespace mongo
