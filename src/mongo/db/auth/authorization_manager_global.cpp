/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace{
    AuthorizationManager* globalAuthManager = NULL;
}

    void setGlobalAuthorizationManager(AuthorizationManager* authManager) {
        fassert(16841, globalAuthManager == NULL);
        globalAuthManager = authManager;
    }

    void clearGlobalAuthorizationManager() {
        fassert(16843, globalAuthManager != NULL);
        globalAuthManager = NULL;
    }

    AuthorizationManager* getGlobalAuthorizationManager() {
        fassert(16842, globalAuthManager != NULL);
        return globalAuthManager;
    }

} // namespace mongo
