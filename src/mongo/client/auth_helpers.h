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
#include "mongo/client/dbclientinterface.h"

namespace mongo {
namespace auth {

    /**
     * Retrieves the schema version of the persistent data describing users and roles from the
     * remote server connected to with conn.
     */
    Status getRemoteStoredAuthorizationVersion(DBClientBase* conn, int* outVersion);

    /**
     * Name of the server parameter used to report the auth schema version (via getParameter).
     */
    extern const std::string schemaVersionServerParameter;

}  // namespace auth
}  // namespace mongo
