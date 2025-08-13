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

#include "mongo/db/user_write_block/write_block_bypass.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {
static const auto getWriteBlockBypass = OperationContext::declareDecoration<WriteBlockBypass>();
}

bool WriteBlockBypass::isWriteBlockBypassEnabled() const {
    return _writeBlockBypassEnabled;
}

void WriteBlockBypass::setFromMetadata(OperationContext* opCtx, boost::optional<bool> val) {
    auto as = AuthorizationSession::get(opCtx->getClient());

    // The caller should ensure those preconditions are met
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(
        !val.has_value() ||
        as->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(as->getUserTenantId()), ActionType::internal));

    // If the mayBypassWriteBlocking field is set, set our state from that field.
    // Otherwise, set our state based on the AuthorizationSession state.
    set(val.has_value() ? *val : as->mayBypassWriteBlockingMode());
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
