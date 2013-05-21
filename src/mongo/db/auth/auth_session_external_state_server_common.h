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
     * The implementation of AuthExternalState functionality common to mongod and mongos.
     */
    class AuthSessionExternalStateServerCommon : public AuthSessionExternalState {
        MONGO_DISALLOW_COPYING(AuthSessionExternalStateServerCommon);

    public:
        virtual ~AuthSessionExternalStateServerCommon();

        virtual bool shouldIgnoreAuthChecks() const;

    protected:
        AuthSessionExternalStateServerCommon();

        // Checks whether or not localhost connections should be given full access and stores the
        // result in _allowLocalhost.  Currently localhost connections are only given full access
        // if there are no users in the admin database.
        virtual void _checkShouldAllowLocalhost();

    private:

        bool _allowLocalhost;

    };

} // namespace mongo
