// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Check a hello request for "saslSupportedMechs" or "speculativeAuthenticate".
 *
 * This will attach supported mechanisms or invoke the behavior of saslStart/authenticate commands
 * as appropriate.
 */
void handleHelloAuth(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     const HelloCommand& cmd,
                     bool isInitialHandshake,
                     BSONObjBuilder* result);

}  // namespace mongo
