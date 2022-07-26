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

#include "mongo/platform/basic.h"

#include "mongo/db/s/forwardable_operation_metadata.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"

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
        setImpersonatedUserMetadata({{authMetadata->getUsers(), authMetadata->getRoles()}});
    }

    setMayBypassWriteBlocking(WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled());
}

void ForwardableOperationMetadata::setOn(OperationContext* opCtx) const {
    Client* client = opCtx->getClient();
    if (const auto& comment = getComment()) {
        stdx::lock_guard<Client> lk(*client);
        opCtx->setComment(comment.get());
    }

    if (const auto& optAuthMetadata = getImpersonatedUserMetadata()) {
        const auto& authMetadata = optAuthMetadata.get();
        const auto& users = authMetadata.getUsers();
        if (!users.empty() || !authMetadata.getRoles().empty()) {
            fassert(ErrorCodes::InternalError, users.size() == 1);
            AuthorizationSession::get(client)->setImpersonatedUserData(users[0],
                                                                       authMetadata.getRoles());
        }
    }

    WriteBlockBypass::get(opCtx).set(getMayBypassWriteBlocking());
}

}  // namespace mongo
