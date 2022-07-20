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

#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/util/duration.h"

namespace mongo {

/**
 * Not thread safe and should only be called on a single threaded context.
 */
class ReshardingOplogApplierMetrics {
public:
    ReshardingOplogApplierMetrics(ReshardingMetrics* metrics,
                                  boost::optional<ReshardingOplogApplierProgress> progressDoc);

    void onInsertApplied();
    void onUpdateApplied();
    void onDeleteApplied();

    void onBatchRetrievedDuringOplogApplying(Milliseconds elapsed);
    void onOplogLocalBatchApplied(Milliseconds elapsed);
    void onOplogEntriesApplied(int64_t numEntries);
    void onWriteToStashCollections();

    int64_t getInsertsApplied() const;
    int64_t getUpdatesApplied() const;
    int64_t getDeletesApplied() const;
    int64_t getOplogEntriesApplied() const;
    int64_t getWritesToStashCollections() const;

private:
    ReshardingMetrics* _metrics;
    AtomicWord<int64_t> _insertsApplied{0};
    AtomicWord<int64_t> _updatesApplied{0};
    AtomicWord<int64_t> _deletesApplied{0};
    AtomicWord<int64_t> _oplogEntriesApplied{0};
    AtomicWord<int64_t> _writesToStashCollections{0};
};

}  // namespace mongo
