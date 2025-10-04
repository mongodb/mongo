/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/repl/replication_auth.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/internal_auth.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {
namespace {

// Gets the singleton AuthorizationManager object for this server process
AuthorizationManager* getGlobalAuthorizationManager() {
    auto shardService = getGlobalServiceContext()->getService(ClusterRole::ShardServer);
    // We can assert here that a shard Service exists since this
    // should only be called in a replication context.
    invariant(shardService != nullptr);
    AuthorizationManager* globalAuthManager = AuthorizationManager::get(shardService);
    fassert(16842, globalAuthManager != nullptr);
    return globalAuthManager;
}

}  // namespace

Status replAuthenticate(DBClientBase* conn) try {
    if (auth::isInternalAuthSet()) {
        conn->authenticateInternalUser();
    } else if (getGlobalAuthorizationManager()->isAuthEnabled())
        return {ErrorCodes::AuthenticationFailed,
                "Authentication is enabled but no internal authentication data is available."};
    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace repl
}  // namespace mongo
