/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_router.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_acquisition_stats.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/invalidating_lru_cache.h"
#include "mongo/util/read_through_cache.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

/**
 * Contains server/cluster-wide information about Authorization.
 */
class AuthorizationManagerImpl : public AuthorizationManager {
public:
    AuthorizationManagerImpl(Service* service, std::unique_ptr<AuthorizationRouter> authzRouter);

    ~AuthorizationManagerImpl() override;

    std::unique_ptr<AuthorizationSession> makeAuthorizationSession(Client* client) override;

    void setShouldValidateAuthSchemaOnStartup(bool validate) override;

    bool shouldValidateAuthSchemaOnStartup() override;

    void setAuthEnabled(bool enabled) override;

    bool isAuthEnabled() const override;

    OID getCacheGeneration() override;

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) override;

    void notifyDDLOperation(OperationContext* opCtx,
                            StringData op,
                            const NamespaceString& nss,
                            const BSONObj& o,
                            const BSONObj* o2) override;

    StatusWith<UserHandle> acquireUser(OperationContext* opCtx,
                                       std::unique_ptr<UserRequest> userRequest) override;
    StatusWith<UserHandle> reacquireUser(OperationContext* opCtx, const UserHandle& user) override;

    /**
     * Invalidate a user, and repin it if necessary.
     */
    void invalidateUserByName(const UserName& user) override;

    void invalidateUsersFromDB(const DatabaseName& dbname) override;

    void invalidateUsersByTenant(const boost::optional<TenantId>& tenant) override;

    /**
     * Verify role information for users in the $external database and insert updated information
     * into the cache if necessary. Currently, this is only used to refresh LDAP users.
     */
    Status refreshExternalUsers(OperationContext* opCtx) override;

    Status initialize(OperationContext* opCtx) override;

    /**
     * Invalidate the user cache, and repin all pinned users.
     */
    void invalidateUserCache() override;

    std::vector<AuthorizationRouter::CachedUserInfo> getUserCacheInfo() const override;

    /**
     * @brief Get the Authorization Router object
     * Should only be used in test.
     */
    AuthorizationRouter* getAuthorizationRouter_forTest();

private:
    std::unique_ptr<AuthorizationRouter> _authzRouter;

    // True if AuthSchema startup checks should be applied in this AuthorizationManager. Changes to
    // its value are not synchronized, so it should only be set once, at initalization time.
    // Note that since AuthorizationVersion has been removed, this only controls whether system
    // indexes are checked at startup.
    bool _startupAuthSchemaValidation{true};

    // True if access control enforcement is enabled in this AuthorizationManager. Changes to its
    // value are not synchronized, so it should only be set once, at initalization time.
    bool _authEnabled{false};

    // A cache of whether there are any users set up for the cluster.
    AtomicWord<bool> _privilegeDocsExist{false};
};

}  // namespace mongo
