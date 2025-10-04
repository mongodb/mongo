/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <queue>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

namespace mongo {

/**
 * Initialized by adding results to its results queue, it then passes through the results in its
 * queue until the queue is empty.
 */
class RouterStageQueuedData final : public RouterExecStage {
public:
    RouterStageQueuedData(OperationContext* opCtx) : RouterExecStage(opCtx) {}
    ~RouterStageQueuedData() final {}

    StatusWith<ClusterQueryResult> next() final;

    Status releaseMemory() final {
        // It has no children. It cannot do anything to release memory.
        return Status::OK();
    }

    void kill(OperationContext* opCtx) final;

    bool remotesExhausted() const final;

    std::size_t getNumRemotes() const final;

    /**
     * Queues a BSONObj to be returned.
     */
    void queueResult(ClusterQueryResult&& result);

    /**
     * Queues an error response.
     */
    void queueError(Status status);

private:
    std::queue<StatusWith<ClusterQueryResult>> _resultsQueue;
};

}  // namespace mongo
