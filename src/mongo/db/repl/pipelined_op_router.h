// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo::repl {

/**
 * Classifies each oplog entry as pipelined or requiring inline
 * application on the dispatcher, and selects the worker for pipelined entries. Ops on the same
 * document always select the same worker so they are applied in order.
 */
class [[MONGO_MOD_PUBLIC]] PipelinedOpRouter {
public:
    enum class OpClass {
        // Hash-routed to one worker.
        kPipelined,
        // Non-transactional applyOps; its inner ops are each pulled out and hash-routed.
        kPipelinedApplyOps,
        // Applied by the dispatcher thread after draining the workers.
        kRequiresInline,
    };

    explicit PipelinedOpRouter(size_t numWorkers);

    /**
     * Classifies an op as kPipelined, kPipelinedApplyOps, or kRequiresInline based on its op type.
     */
    OpClass classify(const OplogEntry& op);

    /**
     * Returns the worker index for 'op'. Multi-key container ops must be expanded first.
     */
    size_t selectWorker(OperationContext* opCtx, OplogEntry* op);

    size_t numWorkers() const {
        return _numWorkers;
    }

private:
    const size_t _numWorkers;

    // Collection properties used to select a worker. Cleared whenever an op is classified as
    // requiring inline application, since applying it may change the underlying collection
    // properties.
    CachedCollectionProperties _collPropertiesCache;
};

}  // namespace mongo::repl
