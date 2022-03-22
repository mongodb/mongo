/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/write_block_bypass.h"

namespace mongo {

namespace {
static const auto getWriteBlockBypass = OperationContext::declareDecoration<WriteBlockBypass>();
}

bool WriteBlockBypass::isWriteBlockBypassEnabled() const {
    return _writeBlockBypassEnabled;
}

void WriteBlockBypass::setFromMetadata(OperationContext* opCtx, const BSONElement& elem) {
    // If we are in direct client, the bypass state should already be set by the initial call of
    // setFromMetadata.
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }
    if (elem) {
        // If the mayBypassWriteBlocking field is set, then (after ensuring the client is
        // authorized) set our state from that field.
        uassert(6317500,
                "Client is not properly authorized to propagate mayBypassWriteBlocking",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::internal));
        set(elem.Bool());
    } else {
        // Otherwise, set our state based on the AuthorizationSession state.
        set(AuthorizationSession::get(opCtx->getClient())->mayBypassWriteBlockingMode());
    }
}

void WriteBlockBypass::set(bool bypassEnabled) {
    _writeBlockBypassEnabled = bypassEnabled;
}

void WriteBlockBypass::writeAsMetadata(BSONObjBuilder* builder) {
    builder->append(fieldName(), _writeBlockBypassEnabled);
}

WriteBlockBypass& WriteBlockBypass::get(OperationContext* opCtx) {
    return getWriteBlockBypass(opCtx);
}
}  // namespace mongo
