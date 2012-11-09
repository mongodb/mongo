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

#include "mongo/db/auth/auth_external_state_impl.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"

namespace mongo {

    AuthExternalStateImpl::AuthExternalStateImpl(DBClientBase* adminDBConnection) {
        _adminUserExists = AuthorizationManager::hasPrivilegeDocument(adminDBConnection, "admin");
        if (!_adminUserExists) {
            log() << "note: no users configured in admin.system.users, allowing localhost access"
                  << endl;
        }
    }
    AuthExternalStateImpl::~AuthExternalStateImpl() {}

    bool AuthExternalStateImpl::shouldIgnoreAuthChecks() const {
        // TODO: uncomment part that checks if connection is localhost by looking in cc()
        return noauth || (!_adminUserExists /*&& cc().isLocalhost*/) || cc().isGod();
    }

} // namespace mongo
