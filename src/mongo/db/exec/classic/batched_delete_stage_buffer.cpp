/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


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
    invariant(empty());
}
}  // namespace mongo
