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
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/util/debug_util.h"

namespace mongo {

    AuthExternalStateServerCommon::AuthExternalStateServerCommon() {}
    AuthExternalStateServerCommon::~AuthExternalStateServerCommon() {}

    Status AuthExternalStateServerCommon::initialize(DBClientBase* adminDBConnection) {
        if (noauth) {
            return Status::OK();
        }

        try {
            _adminUserExists = AuthorizationManager::hasPrivilegeDocument(adminDBConnection,
                                                                          "admin");
        } catch (DBException& e) {
            return Status(ErrorCodes::InternalError,
                          mongoutils::str::stream() << "An error occurred while checking for the "
                                  "existence of an admin user: " << e.what(),
                          0);
        }
        ONCE {
            if (!_adminUserExists) {
                log() << "note: no users configured in admin.system.users, allowing localhost access"
                      << endl;
            }
        }
        return Status::OK();
    }

    bool AuthExternalStateServerCommon::shouldIgnoreAuthChecks() const {
        return noauth || (!_adminUserExists && cc().getIsLocalHostConnection()) || cc().isGod();
    }

} // namespace mongo
