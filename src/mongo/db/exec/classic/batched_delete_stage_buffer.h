// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/working_set.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <set>
#include <vector>

namespace mongo {

/**
 * Buffers documents staged for a batch delete. A document is represented by working set member id
 * (WorkingSetID). Frees the documents whenever they are removed from the buffer.
 */
class BatchedDeleteStageBuffer {
public:
    BatchedDeleteStageBuffer(WorkingSet* ws);

    size_t size() const {
        return _buffer.size();
    }

    bool empty() const {
        return _buffer.empty();
    }

    void append(WorkingSetID id);

    /**
     * Returns the WorkingSetID associated with the staged document at 'bufferOffset'.
     */
    WorkingSetID at(size_t bufferOffset) const {
        return _buffer.at(bufferOffset);
    }

    /**
     * Removes the last n elements in the buffer. Frees up resources of WorkingSetMembers associated
     * with the removed entries.
     */
    void removeLastN(size_t n);

    /**
     * Erases the subset of 'idsToRemove' that exist in the buffer. Frees up resources of the
     * WorkingSetMembers associated with the removed entries.
     */
    void erase(const std::set<WorkingSetID>& idsToRemove);

    /**
     * Clears the buffer and frees up resources of the WorkingSetMembers associated with the removed
     * entries.
     */
    void clear();


private:
    WorkingSet* _ws;
    std::vector<WorkingSetID> _buffer;
};

}  // namespace mongo
