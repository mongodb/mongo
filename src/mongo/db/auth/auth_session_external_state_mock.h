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
#include "mongo/db/auth/auth_session_external_state.h"

namespace mongo {

    /**
     * Mock of the AuthSessionExternalState class used only for testing.
     */
    class AuthSessionExternalStateMock : public AuthSessionExternalState {
        MONGO_DISALLOW_COPYING(AuthSessionExternalStateMock);

    public:
        AuthSessionExternalStateMock() : _returnValue(false) {}

        virtual bool shouldIgnoreAuthChecks() const {
            return _returnValue;
        }

        void setReturnValueForShouldIgnoreAuthChecks(bool returnValue) {
            _returnValue = returnValue;
        }

        virtual bool _findUser(const std::string& usersNamespace,
                               const BSONObj& query,
                               BSONObj* result) const {
            return false;
        }

        virtual void startRequest() {}

        virtual void onAddAuthorizedPrincipal(Principal*) {}

        virtual void onLogoutDatabase(const std::string& dbname) {}

    private:
        bool _returnValue;
    };

} // namespace mongo
