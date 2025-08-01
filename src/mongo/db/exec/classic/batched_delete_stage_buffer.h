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

#pragma once

#include "mongo/db/exec/classic/working_set.h"

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
