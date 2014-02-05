/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/client/auth_helpers.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/util/md5.hpp"

namespace mongo {
namespace auth {

    const std::string schemaVersionServerParameter = "authSchemaVersion";

    std::string createPasswordDigest(const StringData& username,
                                     const StringData& clearTextPassword) {
        md5digest d;
        {
            md5_state_t st;
            md5_init(&st);
            md5_append(&st, (const md5_byte_t *) username.rawData(), username.size());
            md5_append(&st, (const md5_byte_t *) ":mongo:", 7 );
            md5_append(&st,
                       (const md5_byte_t *) clearTextPassword.rawData(),
                       clearTextPassword.size());
            md5_finish(&st, d);
        }
        return digestToString( d );
    }

    Status getRemoteStoredAuthorizationVersion(DBClientBase* conn, int* outVersion) {
        try {
            BSONObj cmdResult;
            conn->runCommand(
                    "admin",
                    BSON("getParameter" << 1 << schemaVersionServerParameter << 1),
                    cmdResult);
            if (!cmdResult["ok"].trueValue()) {
                std::string errmsg = cmdResult["errmsg"].str();
                if (errmsg == "no option found to get" ||
                    StringData(errmsg).startsWith("no such cmd")) {

                    *outVersion = 1;
                    return Status::OK();
                }
                int code = cmdResult["code"].numberInt();
                if (code == 0) {
                    code = ErrorCodes::UnknownError;
                }
                return Status(ErrorCodes::Error(code), errmsg);
            }
            BSONElement versionElement = cmdResult[schemaVersionServerParameter];
            if (versionElement.eoo())
                return Status(ErrorCodes::UnknownError, "getParameter misbehaved.");
            *outVersion = versionElement.numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    void getUpdateToUpgradeUser(const StringData& sourceDB,
                                const BSONObj& oldUserDoc,
                                BSONObj* query,
                                BSONObj* update) {
        uassert(17387,
                mongoutils::str::stream() << "While preparing to upgrade user doc from the 2.4 "
                        "user data schema to the 2.6 schema, found a user doc with a "
                        "\"credentials\" field, indicating that the doc already has the new "
                        "schema. Make sure that all documents in admin.system.users have the same "
                        "user data schema and that the version document in admin.system.version "
                        "indicates the correct schema version.  User doc found: " <<
                        oldUserDoc.toString(),
                !oldUserDoc.hasField("credentials"));

        std::string oldUserSource;
        uassertStatusOK(bsonExtractStringFieldWithDefault(
                                oldUserDoc,
                                "userSource",
                                sourceDB,
                                &oldUserSource));

        const std::string oldUserName = oldUserDoc["user"].String();
        *query = BSON("_id" << oldUserSource + "." + oldUserName);

        BSONObjBuilder updateBuilder;

        {
            BSONObjBuilder toSetBuilder(updateBuilder.subobjStart("$set"));
            toSetBuilder << "user" << oldUserName << "db" << oldUserSource;
            BSONElement pwdElement = oldUserDoc["pwd"];
            if (!pwdElement.eoo()) {
                toSetBuilder << "credentials" << BSON("MONGODB-CR" << pwdElement.String());
            }
            else if (oldUserSource == "$external") {
                toSetBuilder << "credentials" << BSON("external" << true);
            }
        }
        {
            BSONObjBuilder pushAllBuilder(updateBuilder.subobjStart("$pushAll"));
            BSONArrayBuilder rolesBuilder(pushAllBuilder.subarrayStart("roles"));

            const bool readOnly = oldUserDoc["readOnly"].trueValue();
            const BSONElement rolesElement = oldUserDoc["roles"];
            if (readOnly) {
                // Handles the cases where there is a truthy readOnly field, which is a 2.2-style
                // read-only user.
                if (sourceDB == "admin") {
                    rolesBuilder << BSON("role" << "readAnyDatabase" << "db" << "admin");
                }
                else {
                    rolesBuilder << BSON("role" << "read" << "db" << sourceDB);
                }
            }
            else if (rolesElement.eoo()) {
                // Handles the cases where the readOnly field is absent or falsey, but the
                // user is known to be 2.2-style because it lacks a roles array.
                if (sourceDB == "admin") {
                    rolesBuilder << BSON("role" << "root" << "db" << "admin");
                }
                else {
                    rolesBuilder << BSON("role" << "dbOwner" << "db" << sourceDB);
                }
            }
            else {
                // Handles 2.4-style user documents, with roles arrays and (optionally, in admin db)
                // otherDBRoles objects.
                uassert(17252,
                        "roles field in v2.4 user documents must be an array",
                        rolesElement.type() == Array);
                for (BSONObjIterator oldRoles(rolesElement.Obj());
                     oldRoles.more();
                     oldRoles.next()) {

                    BSONElement roleElement = *oldRoles;
                    rolesBuilder << BSON("role" << roleElement.String() << "db" << sourceDB);
                }

                BSONElement otherDBRolesElement = oldUserDoc["otherDBRoles"];
                if (sourceDB == "admin" && !otherDBRolesElement.eoo()) {
                    uassert(17253,
                            "otherDBRoles field in v2.4 user documents must be an object.",
                            otherDBRolesElement.type() == Object);

                    for (BSONObjIterator otherDBs(otherDBRolesElement.Obj());
                         otherDBs.more();
                         otherDBs.next()) {

                        BSONElement otherDBRoles = *otherDBs;
                        if (otherDBRoles.fieldNameStringData() == "local")
                            continue;
                        uassert(17254,
                                "Member fields of otherDBRoles objects must be arrays.",
                                otherDBRoles.type() == Array);
                        for (BSONObjIterator oldRoles(otherDBRoles.Obj());
                             oldRoles.more();
                             oldRoles.next()) {

                            BSONElement roleElement = *oldRoles;
                            rolesBuilder << BSON("role" << roleElement.String() <<
                                                 "db" << otherDBRoles.fieldNameStringData());
                        }
                    }
                }
            }
        }

        *update = updateBuilder.obj();
    }
}  // namespace auth
}  // namespace mongo
