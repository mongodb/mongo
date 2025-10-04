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

#pragma once

#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata/audit_attrs_gen.h"

#include <vector>

#include <boost/optional.hpp>

namespace mongo::rpc {

/**
 * An OperationContext decoration that contains username and roles data for the currently
 * authenticated user or currently impersonated user. This is used to audit correct user
 * information for an operation.
 */
class AuditUserAttrs : public AuditUserAttrsBase {
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
