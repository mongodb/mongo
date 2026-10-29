// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/classic/batched_delete_stage_buffer.h"

#include "mongo/util/assert_util.h"

#include <algorithm>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

BatchedDeleteStageBuffer::BatchedDeleteStageBuffer(WorkingSet* ws) : _ws(ws) {}

void BatchedDeleteStageBuffer::append(WorkingSetID id) {
    _buffer.emplace_back(id);
}

void BatchedDeleteStageBuffer::removeLastN(size_t n) {
    tassert(6515701,
            fmt::format(
                "Cannot remove '{}' elements - beyond the size of the BatchedDeleteStageBuffer {}",
                n,
                _buffer.size()),
            n <= _buffer.size());
    for (unsigned int i = 0; i < n; i++) {
        _ws->free(_buffer.back());
        _buffer.pop_back();
    }
}

void BatchedDeleteStageBuffer::erase(const std::set<WorkingSetID>& idsToRemove) {
    for (auto& workingSetMemberId : idsToRemove) {
        tassert(6515702,
                fmt::format("Attempted to free member with WorkingSetId '{}', which does not exist "
                            "in the BatchedDeleteStageBuffer",
                            workingSetMemberId),
                std::find(_buffer.begin(), _buffer.end(), workingSetMemberId) != _buffer.end());

        _ws->free(workingSetMemberId);
    }

    _buffer.erase(std::remove_if(_buffer.begin(),
                                 _buffer.end(),
                                 [&](auto& workingSetMemberId) {
                                     return idsToRemove.find(workingSetMemberId) !=
                                         idsToRemove.end();
                                 }),
                  _buffer.end());
}

void BatchedDeleteStageBuffer::clear() {
    for (auto& workingSetMemberId : _buffer) {
        _ws->free(workingSetMemberId);
    }

    _buffer.clear();
}
}  // namespace mongo
