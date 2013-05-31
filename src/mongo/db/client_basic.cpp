/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/client_basic.h"

#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_session.h"

namespace mongo {

    ClientBasic::ClientBasic(AbstractMessagingPort* messagingPort) : _messagingPort(messagingPort) {
    }
    ClientBasic::~ClientBasic() {}

    AuthenticationSession* ClientBasic::getAuthenticationSession() {
        return _authenticationSession.get();
    }

    void ClientBasic::resetAuthenticationSession(
            AuthenticationSession* newSession) {

        _authenticationSession.reset(newSession);
    }

    void ClientBasic::swapAuthenticationSession(
            scoped_ptr<AuthenticationSession>& other) {

        _authenticationSession.swap(other);
    }

    bool ClientBasic::hasAuthorizationSession() const {
        return _authorizationSession.get();
    }

    AuthorizationSession* ClientBasic::getAuthorizationSession() const {
            massert(16481,
                    "No AuthorizationManager has been set up for this connection",
                    hasAuthorizationSession());
            return _authorizationSession.get();
    }

    void ClientBasic::setAuthorizationSession(AuthorizationSession* authorizationSession) {
        massert(16477,
                "An AuthorizationManager has already been set up for this connection",
                !hasAuthorizationSession());
        _authorizationSession.reset(authorizationSession);
    }

}  // namespace mongo
