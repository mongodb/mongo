// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"

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
