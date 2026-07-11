// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {
const auto getReplicaSetWriteBlockBypass =
    OperationContext::declareDecoration<ReplicaSetWriteBlockBypass>();
}

bool ReplicaSetWriteBlockBypass::isEnabled() const {
    return _enabled;
}

void ReplicaSetWriteBlockBypass::setFromMetadata(OperationContext* opCtx,
                                                 boost::optional<bool> val) {
    auto as = AuthorizationSession::get(opCtx->getClient());

    // The caller should ensure those preconditions are met.
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(
        !val.has_value() ||
        as->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(as->getUserTenantId()), ActionType::internal));

    // If the mayBypassReplicaSetWriteBlocking field is set, set our state from that field.
    // Otherwise, set our state based on the AuthorizationSession state.
    set(val.has_value() ? *val : as->mayBypassReplicaSetWriteBlocking());
}

void ReplicaSetWriteBlockBypass::set(bool bypassEnabled) {
    _enabled = bypassEnabled;
}

void ReplicaSetWriteBlockBypass::writeAsMetadata(BSONObjBuilder* builder) {
    builder->append(GenericArguments::kMayBypassReplicaSetWriteBlockingFieldName, _enabled);
}

ReplicaSetWriteBlockBypass& ReplicaSetWriteBlockBypass::get(OperationContext* opCtx) {
    return getReplicaSetWriteBlockBypass(opCtx);
}
}  // namespace mongo
