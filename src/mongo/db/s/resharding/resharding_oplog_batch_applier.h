/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

namespace mongo {

class ReshardingOplogApplicationRules;
class ReshardingOplogSessionApplication;

/**
 * Updates this shard's data based on oplog entries that already executed on some donor shard.
 *
 * Instances of this class are thread-safe.
 */
class ReshardingOplogBatchApplier {
public:
    using OplogBatch = ReshardingOplogBatchPreparer::OplogBatchToApply;

    ReshardingOplogBatchApplier(const ReshardingOplogApplicationRules& crudApplication,
                                const ReshardingOplogSessionApplication& sessionApplication);

    template <bool IsForSessionApplication>
    SemiFuture<void> applyBatch(OplogBatch batch,
                                std::shared_ptr<executor::TaskExecutor> executor,
                                CancellationToken cancelToken,
                                CancelableOperationContextFactory factory) const;

private:
    const ReshardingOplogApplicationRules& _crudApplication;
    const ReshardingOplogSessionApplication& _sessionApplication;
};

}  // namespace mongo
