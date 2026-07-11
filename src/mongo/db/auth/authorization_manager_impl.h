// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
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
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/invalidating_lru_cache.h"
#include "mongo/util/modules.h"
#include "mongo/util/read_through_cache.h"

#include <map>
#include <memory>
#include <mutex>
#include <string_view>
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
                            std::string_view op,
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
    Atomic<bool> _privilegeDocsExist{false};
};

}  // namespace mongo
