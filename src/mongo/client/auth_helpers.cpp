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
}  // namespace auth
}  // namespace mongo
