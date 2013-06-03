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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state_server_common.h"

namespace mongo {

    /**
     * The implementation of AuthzSessionExternalState functionality for mongod.
     */
    class AuthzSessionExternalStateMongod : public AuthzSessionExternalStateServerCommon {
        MONGO_DISALLOW_COPYING(AuthzSessionExternalStateMongod);

    public:
        AuthzSessionExternalStateMongod(AuthorizationManager* authzManager);
        virtual ~AuthzSessionExternalStateMongod();

        virtual bool shouldIgnoreAuthChecks() const;

        virtual void startRequest();

        virtual void onAddAuthorizedPrincipal(Principal*);

        virtual void onLogoutDatabase(const std::string&);

    };

} // namespace mongo
