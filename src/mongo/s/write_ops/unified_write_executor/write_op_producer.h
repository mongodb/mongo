// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/unified_write_executor/write_op.h"
#include "mongo/util/modules.h"

#include <absl/container/btree_set.h>
#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

/**
 * This class returns a set of write ops one at a time with incrementing id. It is possible to mark
 * any returned inactive write op to be active again in order for the producer to return it another
 * time.
 */
class WriteOpProducer {
public:
    WriteOpProducer(WriteCommandRef cmdRef) : _cmdRef(std::move(cmdRef)) {
        size_t numOps = _cmdRef.getNumOps();
        populateActiveIndices(numOps);
    }

    WriteOpProducer(const BatchedCommandRequest& request)
        : WriteOpProducer(WriteCommandRef{request}) {}

    WriteOpProducer(const BulkWriteCommandRequest& request)
        : WriteOpProducer(WriteCommandRef{request}) {}

    /**
     * Peek the current active write op without advancing the internal pointer. Repeated calls
     * return the same write op. When no active write op is left, return empty.
     */
    boost::optional<WriteOp> peekNext();

    /**
     * Mark the current write op as inactive and advance the internal pointer to the next active
     * write op.
     */
    void advance();

    /**
     * This method builds a vector containing all currently active write ops, marks all these ops
     * as inactive, and returns the vector.
     */
    std::vector<WriteOp> consumeAllRemainingOps();

    /**
     * Mark a write op as active. The internal pointer will be updated to the active write op with
     * the lowest id.
     */
    void markOpReprocess(const WriteOp& op);

    void stopProducingOps() {
        _stopProducingOps = true;
    }

protected:
    void populateActiveIndices(size_t numOps) {
        auto hint = _activeIndices.end();

        for (size_t i = 0; i < numOps; ++i) {
            hint = _activeIndices.insert(hint, i);
        }
    }

    const WriteCommandRef _cmdRef;
    absl::btree_set<WriteOpId> _activeIndices;
    bool _stopProducingOps{false};
};

}  // namespace unified_write_executor
}  // namespace mongo
