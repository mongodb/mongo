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

#include "mongo/db/auth/auth_external_state_d.h"

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    AuthExternalStateMongod::AuthExternalStateMongod() {}
    AuthExternalStateMongod::~AuthExternalStateMongod() {}

    Status AuthExternalStateMongod::getPrivilegeDocument(const string& dbname,
                                                         const string& principalName,
                                                         BSONObj* result) {
        Client::GodScope gs;
        Client::ReadContext(dbname + ".system.users");
        DBDirectClient conn;
        return getPrivilegeDocumentOverConnection(&conn, dbname, principalName, result);
    }

    bool AuthExternalStateMongod::hasPrivilegeDocument(const std::string& dbname) const {
        Client::GodScope gs;
        Client::ReadContext(dbname + ".system.users");
        DBDirectClient conn;
        BSONObj result = conn.findOne(dbname + ".system.users", Query());
        return !result.isEmpty();
    }

    bool AuthExternalStateMongod::shouldIgnoreAuthChecks() const {
        return cc().isGod() || AuthExternalStateServerCommon::shouldIgnoreAuthChecks();
    }

} // namespace mongo
