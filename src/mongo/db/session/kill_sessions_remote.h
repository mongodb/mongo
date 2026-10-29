// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * The killSessions killer for running on mongos. It kills matching local ops first, then fans out
 * to all other nodes in the cluster to kill them as well.
 */
SessionKiller::Result killSessionsRemote(OperationContext* opCtx,
                                         const SessionKiller::Matcher& patterns,
                                         SessionKiller::UniformRandomBitGenerator* urbg);

}  // namespace mongo
