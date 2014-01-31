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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/export_macros.h"

namespace mongo {
namespace auth {

    /**
     * Hashes the password so that it can be stored in a user object or used for MONGODB-CR
     * authentication.
     */
    std::string MONGO_CLIENT_API createPasswordDigest(const StringData& username,
                                     const StringData& clearTextPassword);

    /**
     * Retrieves the schema version of the persistent data describing users and roles from the
     * remote server connected to with conn.
     */
    Status getRemoteStoredAuthorizationVersion(DBClientBase* conn, int* outVersion);

    /**
     * Given a schemaVersion24 user document and its source database, return the query and update
     * specifier needed to upsert a schemaVersion26 version of the user.
     */
    void getUpdateToUpgradeUser(const StringData& sourceDB,
                                const BSONObj& oldUserDoc,
                                BSONObj* query,
                                BSONObj* update);

    /**
     * Name of the server parameter used to report the auth schema version (via getParameter).
     */
    extern const std::string schemaVersionServerParameter;

}  // namespace auth
}  // namespace mongo
