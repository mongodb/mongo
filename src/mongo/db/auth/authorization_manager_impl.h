/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/auth/authorization_manager.h"

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
class AuthorizationSession;
class AuthzManagerExternalState;
class OperationContext;
class ServiceContext;
class UserDocumentParser;

/**
 * Contains server/cluster-wide information about Authorization.
 */
class AuthorizationManagerImpl : public AuthorizationManager {
public:
    ~AuthorizationManagerImpl() override;

    AuthorizationManagerImpl();

    struct InstallMockForTestingOrAuthImpl {
        explicit InstallMockForTestingOrAuthImpl() = default;
    };

    AuthorizationManagerImpl(std::unique_ptr<AuthzManagerExternalState> externalState,
                             InstallMockForTestingOrAuthImpl);

    std::unique_ptr<AuthorizationSession> makeAuthorizationSession() override;

    void setShouldValidateAuthSchemaOnStartup(bool validate) override;

    bool shouldValidateAuthSchemaOnStartup() override;

    void setAuthEnabled(bool enabled) override;

    bool isAuthEnabled() const override;

    Status getAuthorizationVersion(OperationContext* opCtx, int* version) override;

    OID getCacheGeneration() override;

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) override;

    Status getUserDescription(OperationContext* opCtx,
                              const UserName& userName,
                              BSONObj* result) override;

    Status getRoleDescription(OperationContext* opCtx,
                              const RoleName& roleName,
                              PrivilegeFormat privilegeFormat,
                              AuthenticationRestrictionsFormat,
                              BSONObj* result) override;

    Status getRolesDescription(OperationContext* opCtx,
                               const std::vector<RoleName>& roleName,
                               PrivilegeFormat privilegeFormat,
                               AuthenticationRestrictionsFormat,
                               BSONObj* result) override;

    Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                    StringData dbname,
                                    PrivilegeFormat privilegeFormat,
                                    AuthenticationRestrictionsFormat,
                                    bool showBuiltinRoles,
                                    std::vector<BSONObj>* result) override;

    Status acquireUser(OperationContext* opCtx,
                       const UserName& userName,
                       User** acquiredUser) override;

    void releaseUser(User* user) override;

    void invalidateUserByName(const UserName& user) override;

    void invalidateUsersFromDB(StringData dbname) override;

    Status initialize(OperationContext* opCtx) override;

    void invalidateUserCache() override;

    Status _initializeUserFromPrivilegeDocument(User* user, const BSONObj& privDoc) override;

    void logOp(OperationContext* opCtx,
               const char* opstr,
               const NamespaceString& nss,
               const BSONObj& obj,
               const BSONObj* patt) override;

private:
    /**
     * Type used to guard accesses and updates to the user cache.
     */
    class CacheGuard;
    friend class AuthorizationManagerImpl::CacheGuard;

    /**
     * Invalidates all User objects in the cache and removes them from the cache.
     * Should only be called when already holding _cacheMutex.
     */
    void _invalidateUserCache_inlock();

    /**
     * Given the objects describing an oplog entry that affects authorization data, invalidates
     * the portion of the user cache that is affected by that operation.  Should only be called
     * with oplog entries that have been pre-verified to actually affect authorization data.
     */
    void _invalidateRelevantCacheData(const char* op,
                                      const NamespaceString& ns,
                                      const BSONObj& o,
                                      const BSONObj* o2);

    /**
     * Updates _cacheGeneration to a new OID
     */
    void _updateCacheGeneration_inlock();

    /**
     * Fetches user information from a v2-schema user document for the named user,
     * and stores a pointer to a new user object into *acquiredUser on success.
     */
    Status _fetchUserV2(OperationContext* opCtx,
                        const UserName& userName,
                        std::unique_ptr<User>* acquiredUser);

    /**
     * True if AuthSchema startup checks should be applied in this AuthorizationManager.
     *
     * Defaults to true.  Changes to its value are not synchronized, so it should only be set
     * at initalization-time.
     */
    bool _startupAuthSchemaValidation;

    /**
     * True if access control enforcement is enabled in this AuthorizationManager.
     *
     * Defaults to false.  Changes to its value are not synchronized, so it should only be set
     * at initalization-time.
     */
    bool _authEnabled;

    /**
     * A cache of whether there are any users set up for the cluster.
     */
    bool _privilegeDocsExist;

    // Protects _privilegeDocsExist
    mutable stdx::mutex _privilegeDocsExistMutex;

    std::unique_ptr<AuthzManagerExternalState> _externalState;

    /**
     * Cached value of the authorization schema version.
     *
     * May be set by acquireUser() and getAuthorizationVersion().  Invalidated by
     * invalidateUserCache().
     *
     * Reads and writes guarded by CacheGuard.
     */
    int _version;

    /**
     * Caches User objects with information about user privileges, to avoid the need to
     * go to disk to read user privilege documents whenever possible.  Every User object
     * has a reference count - the AuthorizationManager must not delete a User object in the
     * cache unless its reference count is zero.
     */
    stdx::unordered_map<UserName, User*> _userCache;

    /**
     * Current generation of cached data.  Updated every time part of the cache gets
     * invalidated.  Protected by CacheGuard.
     */
    OID _cacheGeneration;

    /**
     * True if there is an update to the _userCache in progress, and that update is currently in
     * the "fetch phase", during which it does not hold the _cacheMutex.
     *
     * Manipulated via CacheGuard.
     */
    bool _isFetchPhaseBusy;

    /**
     * Protects _userCache, _cacheGeneration, _version and _isFetchPhaseBusy.  Manipulated
     * via CacheGuard.
     */
    stdx::mutex _cacheMutex;

    /**
     * Condition used to signal that it is OK for another CacheGuard to enter a fetch phase.
     * Manipulated via CacheGuard.
     */
    stdx::condition_variable _fetchPhaseIsReady;
};
}  // namespace mongo
