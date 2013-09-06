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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
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
