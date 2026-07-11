// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata/audit_attrs_gen.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional.hpp>

namespace mongo::rpc {

/**
 * An OperationContext decoration that contains username and roles data for the currently
 * authenticated user or currently impersonated user. This is used to audit correct user
 * information for an operation.
 */
class [[MONGO_MOD_PUBLIC]] AuditUserAttrs : public AuditUserAttrsBase {
public:
    AuditUserAttrs(UserName userName, std::vector<RoleName> roleNames, bool isImpersonating)
        : AuditUserAttrsBase(std::move(userName), std::move(roleNames), isImpersonating) {}
    explicit AuditUserAttrs(const BSONObj& obj);

    static boost::optional<AuditUserAttrs> get(OperationContext* opCtx);
    static void set(OperationContext* opCtx, AuditUserAttrs attrs);
    static void set(OperationContext* opCtx,
                    UserName userName,
                    std::vector<RoleName> roleNames,
                    bool isImpersonating) {
        set(opCtx, AuditUserAttrs(std::move(userName), std::move(roleNames), isImpersonating));
    }
    static void resetToAuthenticatedUser(OperationContext* opCtx);

    static boost::optional<AuditUserAttrs> get(Client* client) {
        tassert(9791302, "Must have client in AuditUserAttrs::get", client);
        return get(client->getOperationContext());
    }
    static void resetToAuthenticatedUser(Client* client) {
        tassert(9791303, "Must have client in AuditUserAttrs::resetToAuthenticatedUser", client);
        if (auto* opCtx = client->getOperationContext()) {
            resetToAuthenticatedUser(opCtx);
        }
    }
};

}  // namespace mongo::rpc
