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

#pragma once

#include <boost/scoped_ptr.hpp>

#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/message_port.h"

namespace mongo {

    class AuthenticationInfo;
    class AuthenticationSession;
    class AuthorizationSession;

    /**
     * this is the base class for Client and ClientInfo
     * Client is for mongod
     * ClientInfo is for mongos
     * They should converge slowly
     * The idea is this has the basic api so that not all code has to be duplicated
     */
    class ClientBasic : boost::noncopyable {
    public:
        virtual ~ClientBasic();
        AuthenticationSession* getAuthenticationSession();
        void resetAuthenticationSession(AuthenticationSession* newSession);
        void swapAuthenticationSession(boost::scoped_ptr<AuthenticationSession>& other);

        bool hasAuthorizationSession() const;
        AuthorizationSession* getAuthorizationSession() const;
        void setAuthorizationSession(AuthorizationSession* authorizationSession);

        bool getIsLocalHostConnection() {
            if (!hasRemote()) {
                return false;
            }
            return getRemote().isLocalHost();
        }

        virtual bool hasRemote() const { return _messagingPort; }
        virtual HostAndPort getRemote() const {
            verify( _messagingPort );
            return _messagingPort->remote();
        }
        AbstractMessagingPort * port() const { return _messagingPort; }

        static ClientBasic* getCurrent();
        static bool hasCurrent();

    protected:
        ClientBasic(AbstractMessagingPort* messagingPort);

    private:
        boost::scoped_ptr<AuthenticationSession> _authenticationSession;
        boost::scoped_ptr<AuthorizationSession> _authorizationSession;
        AbstractMessagingPort* const _messagingPort;
    };
}
