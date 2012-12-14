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

#include "mongo/db/auth/auth_external_state_server_common.h"

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/util/debug_util.h"

namespace mongo {

    AuthExternalStateServerCommon::AuthExternalStateServerCommon() : _allowLocalhost(false) {}
    AuthExternalStateServerCommon::~AuthExternalStateServerCommon() {}

    void AuthExternalStateServerCommon::_checkShouldAllowLocalhost() {
        // TODO: cache if admin user exists and if it once existed don't query admin.system.users
        _allowLocalhost = !_hasPrivilegeDocument("admin");
        if (_allowLocalhost) {
            ONCE {
                log() << "note: no users configured in admin.system.users, allowing localhost "
                        "access" << std::endl;
            }
        }
    }

    bool AuthExternalStateServerCommon::shouldIgnoreAuthChecks() const {
        ClientBasic* client = ClientBasic::getCurrent();
        return noauth || (client->getIsLocalHostConnection() && _allowLocalhost);
    }

} // namespace mongo
