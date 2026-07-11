// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/command_can_run_here.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

namespace mongo {

bool commandCanRunHere(OperationContext* opCtx,
                       const DatabaseName& dbName,
                       const Command* command,
                       bool inMultiDocumentTransaction) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbName))
        return true;  // primary: always ok
    if (!opCtx->writesAreReplicated())
        return true;  // standalone: always ok
    if (inMultiDocumentTransaction)
        return false;  // Transactions are not allowed on secondaries.
    switch (command->secondaryAllowed(opCtx->getServiceContext())) {
        case Command::AllowedOnSecondary::kAlways:
            return true;
        case Command::AllowedOnSecondary::kNever:
            return false;
        case Command::AllowedOnSecondary::kOptIn:
            // Don't reject reads to localDb collections if the db is not writeable
            // regardless of secondary read preference settings.
            if (dbName.isLocalDB())
                return true;
            // Did the user opt in?
            return ReadPreferenceSetting::get(opCtx).canRunOnSecondary();
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
