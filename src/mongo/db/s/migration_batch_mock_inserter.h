// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_batch_inserter.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

#pragma once

namespace mongo {

class MigrationBatchMockInserter {
public:
    void run(Status status) const {
        // Run is passed in a non-ok status if this function runs inline.
        // That happens if we schedule this task on a ThreadPool that is
        // already shutdown.  We should never do that.  Therefore,
        // we assert that here.
        invariant(status);
    }
    MigrationBatchMockInserter(OperationContext*,
                               OperationContext*,
                               BSONObj,
                               NamespaceString,
                               ChunkRange,
                               WriteConcernOptions,
                               UUID,
                               std::shared_ptr<MigrationCloningProgressSharedState>,
                               UUID,
                               TicketHolder*) {}

    static void onCreateThread(const std::string& threadName) {}

private:
    BSONObj _batch;
};

}  // namespace mongo
