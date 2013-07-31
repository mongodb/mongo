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
#include "mongo/s/type_database.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    static Status userNotFoundStatus(ErrorCodes::UserNotFound, "User not found");
}

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

    Status AuthzManagerExternalStateMongos::_findUser(const string& usersNamespace,
                                                      const BSONObj& query,
                                                      BSONObj* result) const {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(usersNamespace));
            *result = conn->get()->findOne(usersNamespace, query).getOwned();
            conn->done();
            if (result->isEmpty()) {
                return userNotFoundStatus;
            }
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::insertPrivilegeDocument(const string& dbname,
                                                                    const BSONObj& userObj) const {
        try {
            string userNS = dbname + ".system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

            conn->get()->insert(userNS, userObj);

            // 30 second timeout for w:majority
            BSONObj res = conn->get()->getLastErrorDetailed(false, false, -1, 30*1000);
            string errstr = conn->get()->getLastErrorString(res);
            conn->done();

            if (errstr.empty()) {
                return Status::OK();
            }
            if (res.hasField("code") && res["code"].Int() == ASSERT_ID_DUPKEY) {
                return Status(ErrorCodes::DuplicateKey,
                              mongoutils::str::stream() << "User \"" << userObj["user"].String() <<
                                     "\" already exists on database \"" << dbname << "\"");
            }
            return Status(ErrorCodes::UserModificationFailed, errstr);
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::updatePrivilegeDocument(
            const UserName& user, const BSONObj& updateObj) const {
        try {
            string userNS = mongoutils::str::stream() << user.getDB() << ".system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

            conn->get()->update(userNS,
                                QUERY("user" << user.getUser() << "userSource" << BSONNULL),
                                updateObj);

            // 30 second timeout for w:majority
            BSONObj res = conn->get()->getLastErrorDetailed(false, false, -1, 30*1000);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

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
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::removePrivilegeDocuments(const string& dbname,
                                                                     const BSONObj& query) const {
        try {
            string userNS = dbname + ".system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(userNS));

            conn->get()->remove(userNS, query);

            // 30 second timeout for w:majority
            BSONObj res = conn->get()->getLastErrorDetailed(false, false, -1, 30*1000);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

            if (!err.empty()) {
                return Status(ErrorCodes::UserModificationFailed, err);
            }

            int numUpdated = res["n"].numberInt();
            if (numUpdated == 0) {
                return Status(ErrorCodes::UserNotFound,
                              mongoutils::str::stream() << "No users found on database \"" << dbname
                                      << "\" matching query: " << query.toString());
            }
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getAllDatabaseNames(
            std::vector<std::string>* dbnames) const {
        try {
            scoped_ptr<ScopedDbConnection> conn(
                    getConnectionForUsersCollection(DatabaseType::ConfigNS));
            auto_ptr<DBClientCursor> c = conn->get()->query(DatabaseType::ConfigNS, Query());

            while (c->more()) {
                DatabaseType dbInfo;
                std::string errmsg;
                if (!dbInfo.parseBSON( c->nextSafe(), &errmsg) || !dbInfo.isValid( &errmsg )) {
                    return Status(ErrorCodes::FailedToParse, errmsg);
                }
                dbnames->push_back(dbInfo.getName());
            }
            conn->done();
            dbnames->push_back("config"); // config db isn't listed in config.databases
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getAllV1PrivilegeDocsForDB(
            const std::string& dbname, std::vector<BSONObj>* privDocs) const {
        try {
            std::string usersNamespace = dbname + ".system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForUsersCollection(usersNamespace));
            auto_ptr<DBClientCursor> c = conn->get()->query(usersNamespace, Query());

            while (c->more()) {
                privDocs->push_back(c->nextSafe().getOwned());
            }
            conn->done();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

} // namespace mongo
