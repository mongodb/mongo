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

#include "mongo/db/auth/authz_manager_external_state_d.h"

#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    AuthzManagerExternalStateMongod::AuthzManagerExternalStateMongod() {}
    AuthzManagerExternalStateMongod::~AuthzManagerExternalStateMongod() {}

    bool AuthzManagerExternalStateMongod::_findUser(const string& usersNamespace,
                                                    const BSONObj& query,
                                                    BSONObj* result) const {
        Client::GodScope gs;
        Client::ReadContext ctx(usersNamespace);

        return Helpers::findOne(usersNamespace, query, *result);
    }

} // namespace mongo
