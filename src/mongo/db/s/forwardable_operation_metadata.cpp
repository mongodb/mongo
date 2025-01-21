/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/forwardable_operation_metadata.h"

#include <boost/optional.hpp>
#include <mutex>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/metadata/audit_client_attrs.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/rpc/metadata/impersonated_user_metadata_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ForwardableOperationMetadata::ForwardableOperationMetadata(const BSONObj& obj) {
    ForwardableOperationMetadataBase::parseProtected(
        IDLParserContext("ForwardableOperationMetadataBase"), obj);
}

ForwardableOperationMetadata::ForwardableOperationMetadata(OperationContext* opCtx) {
    if (auto optComment = opCtx->getComment()) {
        setComment(optComment->wrap());
    }

    if (const auto authMetadata = rpc::getImpersonatedUserMetadata(opCtx)) {
        if (authMetadata->getUser()) {
            AuthenticationMetadata metadata;
            metadata.setUser(authMetadata->getUser().get());
            metadata.setRoles(authMetadata->getRoles());
            setImpersonatedUserMetadata(metadata);
        }
    }

    if (auto auditClientAttrs = rpc::AuditClientAttrs::get(opCtx->getClient())) {
        setAuditClientMetadata(std::move(auditClientAttrs));
    }

    boost::optional<StringData> originalSecurityToken = boost::none;
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    if (vts != boost::none && !vts->getOriginalToken().empty()) {
        originalSecurityToken = vts->getOriginalToken();
    }
    setValidatedTenancyScopeToken(originalSecurityToken);

    setMayBypassWriteBlocking(WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled());
}

void ForwardableOperationMetadata::setOn(OperationContext* opCtx) const {
    Client* client = opCtx->getClient();
    if (const auto& comment = getComment()) {
        stdx::lock_guard<Client> lk(*client);
        opCtx->setComment(comment.value());
    }

    if (const auto& optAuthMetadata = getImpersonatedUserMetadata()) {
        const auto& authMetadata = optAuthMetadata.value();
        UserName username(authMetadata.getUser().value_or(UserName()));

        if (!authMetadata.getRoles().empty()) {
            AuthorizationSession::get(client)->setImpersonatedUserData(username,
                                                                       authMetadata.getRoles());
        }
    }

    if (const auto& optAuditClientMetadata = getAuditClientMetadata()) {
        rpc::AuditClientAttrs::set(client, optAuditClientMetadata.value());
    }

    WriteBlockBypass::get(opCtx).set(getMayBypassWriteBlocking());
    boost::optional<auth::ValidatedTenancyScope> validatedTenancyScope = boost::none;
    const auto originalToken = getValidatedTenancyScopeToken();
    if (originalToken != boost::none && !originalToken->empty()) {
        validatedTenancyScope = auth::ValidatedTenancyScopeFactory::parse(client, *originalToken);
    }
    auth::ValidatedTenancyScope::set(opCtx, validatedTenancyScope);
}

}  // namespace mongo
