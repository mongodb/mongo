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

#include "mongo/db/auth/authz_manager_external_state_s.h"

#include <string>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() {}
    AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() {}

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

    bool AuthzManagerExternalStateMongos::_findUser(const string& usersNamespace,
                                                    const BSONObj& query,
                                                    BSONObj* result) const {
        scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(usersNamespace));
        *result = conn->get()->findOne(usersNamespace, query).getOwned();
        conn->done();
        return !result->isEmpty();
    }

    Status AuthzManagerExternalStateMongos::insertPrivilegeDocument(const string& dbname,
                                                                    const BSONObj& userObj) const {
        string userNS = dbname + ".system.users";
        scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

        conn->get()->insert(userNS, userObj);

        // 30 second timeout for w:majority
        BSONObj res = conn->get()->getLastErrorDetailed(false, false, -1, 30*1000);
        string errstr = conn->get()->getLastErrorString(res);
        if (errstr.empty()) {
            return Status::OK();
        }
        if (res.hasField("code") && res["code"].Int() == ASSERT_ID_DUPKEY) {
            return Status(ErrorCodes::DuplicateKey,
                          mongoutils::str::stream() << "User \"" << userObj["user"].String() <<
                                 "\" already exists on database \"" << dbname << "\"");
        }
        return Status(ErrorCodes::UserModificationFailed, errstr);
    }

    Status AuthzManagerExternalStateMongos::updatePrivilegeDocument(
            const UserName& user, const BSONObj& updateObj) const {
        string userNS = mongoutils::str::stream() << user.getDB() << ".system.users";
        scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

        conn->get()->update(userNS,
                            QUERY("user" << user.getUser() << "userSource" << BSONNULL),
                            updateObj);

        // 30 second timeout for w:majority
        BSONObj res = conn->get()->getLastErrorDetailed(false, false, -1, 30*1000);
        string err = conn->get()->getLastErrorString(res);
        if (!err.empty()) {
            return Status(ErrorCodes::UserModificationFailed, err);
        }

        int numUpdated = res["n"].numberInt();
        dassert(numUpdated <= 1 && numUpdated >= 0);
        if (numUpdated == 0) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream() << "User " << user.getFullName() <<
                                  " not found");
        }

        return Status::OK();
    }

    void AuthzManagerExternalStateMongos::getAllDatabaseNames(
            std::vector<std::string>* dbnames) const {
        // TODO(spencer): NOT YET IMPLEMENTED
        fassertFailed(16964);
    }

    std::vector<BSONObj> AuthzManagerExternalStateMongos::getAllV1PrivilegeDocsForDB(
            const std::string& dbname) const {
        // TODO(spencer): NOT YET IMPLEMENTED
        fassertFailed(16965);
    }


} // namespace mongo
