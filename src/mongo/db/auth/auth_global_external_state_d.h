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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/auth_global_external_state.h"

namespace mongo {

    /**
     * The implementation of AuthGlobalExternalState functionality for mongod.
     */
    class AuthGlobalExternalStateMongod : public AuthGlobalExternalState {
        MONGO_DISALLOW_COPYING(AuthGlobalExternalStateMongod);

    public:
        AuthGlobalExternalStateMongod();
        virtual ~AuthGlobalExternalStateMongod();

    protected:
        virtual bool _findUser(const string& usersNamespace,
                               const BSONObj& query,
                               BSONObj* result) const;
    };

} // namespace mongo
