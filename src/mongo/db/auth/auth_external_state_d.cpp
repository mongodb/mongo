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

#include "mongo/db/auth/auth_external_state_d.h"

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    AuthExternalStateMongod::AuthExternalStateMongod() {}
    AuthExternalStateMongod::~AuthExternalStateMongod() {}

    void AuthExternalStateMongod::startRequest() {
        if (!Lock::isLocked()) {
            _checkShouldAllowLocalhost();
        }
    }

    bool AuthExternalStateMongod::_findUser(const string& usersNamespace,
                                            const BSONObj& query,
                                            BSONObj* result) const {
        Client::GodScope gs;
        Client::ReadContext ctx(usersNamespace);

        return Helpers::findOne(usersNamespace, query, *result);
    }

    bool AuthExternalStateMongod::shouldIgnoreAuthChecks() const {
        return cc().isGod() || AuthExternalStateServerCommon::shouldIgnoreAuthChecks();
    }

    void AuthExternalStateMongod::onAddAuthorizedPrincipal(Principal*) {
        // invalidate all thread-local JS scopes due to new user authentication
        if (globalScriptEngine)
            globalScriptEngine->threadDone();
    }

    void AuthExternalStateMongod::onLogoutDatabase(const std::string&) {
        // invalidate all thread-local JS scopes due to logout
        if (globalScriptEngine)
            globalScriptEngine->threadDone();
    }

} // namespace mongo
