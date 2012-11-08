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
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/external_state.h"

namespace mongo {

    /**
     * The actual implementation of AuthExternalState
     */
    class AuthExternalStateImpl : public AuthExternalState {
        MONGO_DISALLOW_COPYING(AuthExternalStateImpl);

    public:

        // adminDBConnection is a connection that can be used to access the admin database.  It is
        // used to determine if there are any admin users configured for the cluster, and thus if
        // localhost connections should be given special admin access.
        // adminDBConnection is used only in the constructor, no pointer to it is stored, so it
        // can be deleted as soon as the constructor returns.
        AuthExternalStateImpl(DBClientBase* adminDBConnection);
        virtual ~AuthExternalStateImpl();

        // Returns true if this connection should be treated as if it has full access to do
        // anything, regardless of the current auth state.  Currently the reasons why this could be
        // are that auth isn't enabled, the connection is from localhost and there are admin users,
        // or the connection is a "god" connection.
        virtual bool shouldIgnoreAuthChecks() const;

    private:

        bool _adminUserExists;
    };

} // namespace mongo
