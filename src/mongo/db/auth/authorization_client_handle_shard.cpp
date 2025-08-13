/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_client_handle_shard.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_router.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl
namespace mongo {

namespace {
class AuthzCollection {
public:
    enum class AuthzCollectionType {
        kNone,
        kUsers,
        kRoles,
        kVersion,
        kAdmin,
    };

    AuthzCollection() = default;
    explicit AuthzCollection(const NamespaceString& nss) : _tenant(nss.tenantId()) {
        // Capture events regardless of what Tenant they occured in,
        // invalidators will purge cache on a per-tenant basis as needed.
        auto db = nss.dbName();
        auto coll = nss.coll();
        if (!db.isAdminDB()) {
            return;
        }

        // System-only collections.
        if (coll == NamespaceString::kServerConfigurationNamespace.coll()) {
            _type = AuthzCollectionType::kVersion;
            return;
        }

        if (coll == NamespaceString::kAdminCommandNamespace.coll()) {
            _type = AuthzCollectionType::kAdmin;
            return;
        }

        if (coll == NamespaceString::kSystemUsers) {
            // admin.system.users or {tenantID}_admin.system.users
            _type = AuthzCollectionType::kUsers;
            return;
        }

        if (coll == NamespaceString::kSystemRoles) {
            // admin.system.roles or {tenantID}_admin.system.roles
            _type = AuthzCollectionType::kRoles;
            return;
        }
    }

    operator bool() const {
        return _type != AuthzCollectionType::kNone;
    }

    bool isPrivilegeCollection() const {
        return (_type == AuthzCollectionType::kUsers) || (_type == AuthzCollectionType::kRoles);
    }

    AuthzCollectionType getType() const {
        return _type;
    }

    const boost::optional<TenantId>& tenantId() const {
        return _tenant;
    }

private:
    AuthzCollectionType _type = AuthzCollectionType::kNone;
    boost::optional<TenantId> _tenant;
};

constexpr auto kOpInsert = "i"_sd;
constexpr auto kOpUpdate = "u"_sd;
constexpr auto kOpDelete = "d"_sd;

using InvalidateFn = std::function<void()>;

/**
 * When we are currently in a WriteUnitOfWork, invalidation of the user cache must wait until
 * after the operation causing the invalidation commits. This is because if in a different thread,
 * the cache is read after invalidation but before the related commit occurs, the cache will be
 * populated with stale data until the next invalidation.
 */
void invalidateUserCacheOnCommit(OperationContext* opCtx, InvalidateFn invalidate) {
    auto unit = shard_role_details::getRecoveryUnit(opCtx);
    if (unit && unit->inUnitOfWork()) {
        LOGV2_DEBUG(9349700,
                    5,
                    "In WriteUnitOfWork, deferring user cache invalidation to onCommit handler");
        unit->onCommit([invalidate = std::move(invalidate)](OperationContext* opCtx,
                                                            boost::optional<Timestamp>) {
            LOGV2_DEBUG(9349701, 3, "Invalidating user cache in onCommit handler");
            invalidate();
        });
    } else {
        LOGV2_DEBUG(9349702, 3, "Not in WriteUnitOfWork, invalidating user cache immediately");
        invalidate();
    }
}

void _invalidateUserCache(OperationContext* opCtx,
                          AuthorizationRouter* authzRouter,
                          StringData op,
                          AuthzCollection coll,
                          const BSONObj& o,
                          const BSONObj* o2) {
    if ((coll.getType() == AuthzCollection::AuthzCollectionType::kUsers) &&
        ((op == kOpInsert) || (op == kOpUpdate) || (op == kOpDelete))) {
        const BSONObj* src = (op == kOpUpdate) ? o2 : &o;
        auto id = (*src)["_id"].str();
        auto splitPoint = id.find('.');
        if (splitPoint == std::string::npos) {
            LOGV2_WARNING(23749,
                          "Invalidating user cache based on user being updated failed, will "
                          "invalidate the entire cache instead",
                          "error"_attr =
                              Status(ErrorCodes::FailedToParse,
                                     str::stream() << "_id entries for user documents must be of "
                                                      "the form <dbname>.<username>.  Found: "
                                                   << id));

            invalidateUserCacheOnCommit(opCtx,
                                        [authzRouter]() { authzRouter->invalidateUserCache(); });
            return;
        }
        UserName userName(id.substr(splitPoint + 1), id.substr(0, splitPoint), coll.tenantId());
        invalidateUserCacheOnCommit(opCtx, [userName = std::move(userName), authzRouter]() {
            authzRouter->invalidateUserByName(userName);
        });
    } else if (const auto& tenant = coll.tenantId()) {
        invalidateUserCacheOnCommit(opCtx, [tenantId = tenant.value(), authzRouter]() {
            authzRouter->invalidateUsersByTenant(tenantId);
        });
    } else {
        invalidateUserCacheOnCommit(opCtx, [authzRouter]() { authzRouter->invalidateUserCache(); });
    }
}

}  // namespace

StatusWith<BSONObj> AuthorizationClientHandleShard::runAuthorizationReadCommand(
    OperationContext* opCtx, const DatabaseName& dbname, const BSONObj& command) {
    DBDirectClient client(opCtx);
    BSONObj response;
    if (!client.runCommand(dbname, std::move(command), response)) {
        return getErrorStatusFromCommandResult(response);
    }

    return response;
}

void AuthorizationClientHandleShard::notifyDDLOperation(OperationContext* opCtx,
                                                        AuthorizationRouter* authzRouter,
                                                        StringData op,
                                                        const NamespaceString& nss,
                                                        const BSONObj& o,
                                                        const BSONObj* o2) {
    AuthzCollection coll(nss);
    if (!coll) {
        return;
    }

    _invalidateUserCache(opCtx, authzRouter, op, coll, o, o2);
}

}  // namespace mongo
