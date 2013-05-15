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

#include "mongo/db/auth/auth_external_state_s.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/grid.h"

namespace mongo {

    AuthExternalStateMongos::AuthExternalStateMongos() {}
    AuthExternalStateMongos::~AuthExternalStateMongos() {}

    void AuthExternalStateMongos::onAddAuthorizedPrincipal(Principal*) { }

    void AuthExternalStateMongos::onLogoutDatabase(const std::string&) { }

    void AuthExternalStateMongos::startRequest() {
        _checkShouldAllowLocalhost();
    }

    namespace {
        ScopedDbConnection* getConnectionForUsersCollection(const std::string& ns) {
            //
            // Note: The connection mechanism here is *not* ideal, and should not be used elsewhere.
            // If the primary for the collection moves, this approach may throw rather than handle
            // version exceptions.
            //

            DBConfigPtr config = grid.getDBConfig(ns);
            Shard s = config->getShard(ns);

            return new ScopedDbConnection(s.getConnString(), 30.0);
        }
    }

    bool AuthExternalStateMongos::_findUser(const string& usersNamespace,
                                            const BSONObj& query,
                                            BSONObj* result) const {
        scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(usersNamespace));
        *result = conn->get()->findOne(usersNamespace, query).getOwned();
        conn->done();
        return !result->isEmpty();
    }

} // namespace mongo
