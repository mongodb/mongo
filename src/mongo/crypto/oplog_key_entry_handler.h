// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"

namespace mongo {

class OplogKeyEntryHandler {
public:
    virtual ~OplogKeyEntryHandler() = default;

    /**
     * Attempts to apply a custom oplog entry. Checks whether the entry is a Key, and handles
     * the entry using custom logic.
     */
    virtual Status applyOplogEntry(OperationContext* opCtx, const repl::OplogEntry& oplogEntry);

    static void set(ServiceContext* serviceContext, std::unique_ptr<OplogKeyEntryHandler> handler);

    static OplogKeyEntryHandler* get(ServiceContext* serviceContext);
};

}  // namespace mongo
