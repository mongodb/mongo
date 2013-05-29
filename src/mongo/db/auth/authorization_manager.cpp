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

#include "mongo/db/auth/authorization_manager.h"

#include <string>

namespace mongo {

    AuthInfo::AuthInfo() {
        user = "__system";
    }
    AuthInfo internalSecurity;

    const std::string AuthorizationManager::SERVER_RESOURCE_NAME = "$SERVER";
    const std::string AuthorizationManager::CLUSTER_RESOURCE_NAME = "$CLUSTER";
    const std::string AuthorizationManager::USER_NAME_FIELD_NAME = "user";
    const std::string AuthorizationManager::USER_SOURCE_FIELD_NAME = "userSource";
    const std::string AuthorizationManager::PASSWORD_FIELD_NAME = "pwd";

    bool AuthorizationManager::_doesSupportOldStylePrivileges = true;
    bool AuthorizationManager::_authEnabled = false;

    AuthorizationManager::AuthorizationManager(AuthGlobalExternalState* globalExternalState) :
            _globalExternalState(globalExternalState) {}

    AuthGlobalExternalState* AuthorizationManager::getGlobalExternalState() const {
        return _globalExternalState.get();
    }

    void AuthorizationManager::setSupportOldStylePrivilegeDocuments(bool enabled) {
        _doesSupportOldStylePrivileges = enabled;
    }

    bool AuthorizationManager::getSupportOldStylePrivilegeDocuments() {
        return _doesSupportOldStylePrivileges;
    }

    void AuthorizationManager::setAuthEnabled(bool enabled) {
        _authEnabled = enabled;
    }

    bool AuthorizationManager::isAuthEnabled() {
        return _authEnabled;
    }

} // namespace mongo
