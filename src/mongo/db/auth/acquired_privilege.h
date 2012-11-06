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

#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/privilege.h"

namespace mongo {

    /**
     * A representation that a given principal has the permission to perform a set of actions on a
     * specific resource.
     */
    class AcquiredPrivilege {
    public:

        AcquiredPrivilege(const Privilege& privilege, Principal* principal) :
            _privilege(privilege), _principal(principal) {}
        ~AcquiredPrivilege() {}

        const Principal* getPrincipal() const { return _principal; }

        const Privilege& getPrivilege() const { return _privilege; }

    private:

        Privilege _privilege;
        Principal* _principal;
    };

} // namespace mongo
