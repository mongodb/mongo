/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_visibility_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/oplog_truncate_markers.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <vector>

#include <boost/optional/optional.hpp>


namespace MONGO_MOD_PUB mongo {

// Stores the total time an operation spends with an uncommitted oplog slot held open. Indicator
// that an operation is holding back replication by causing oplog holes to remain open for
// unusual amounts of time.
class OplogSlotTimeContext {
    int64_t _batchCount = 0;
    boost::optional<Timer> _timer;
    AtomicWord<int64_t> _totalOplogSlotDurationMicros;

public:
    /**
     * Increment number of allocated slot batches(via getNextOpTimes) within single unit of work
     * resets timer on first batch
     */
    void incBatchCount() {
        if (!_batchCount++) {
            if (!_timer) {
                _timer.emplace();
            } else {
                _timer->reset();
            }
        }
    }

    /**
     * Decrement number of allocated slot batches(happens when batch is committed/rolled back)
     * update duration on last batch
     */
    void decBatchCount() {
        invariant(_timer != boost::none);
        if (!--_batchCount) {
            _totalOplogSlotDurationMicros.fetchAndAdd(
                durationCount<Microseconds>(_timer->elapsed()));
            // no need to reset timer here, will be reset in subsequent increment
        }
    }

    /**
     * Retrieves total number of Microseconds there was an existing unit of work within same
     * operation context
     */
    Microseconds getTotalMicros() const {
        return Microseconds(_totalOplogSlotDurationMicros.loadRelaxed());
    }

    auto getBatchCount() const {
        return _batchCount;
    }

    const auto& getTimer() const {
        invariant(_timer != boost::none);
        return *_timer;
    }

    void setTickSource(TickSource* ts) {
        _timer.emplace(ts);
    }
};


/**
 * This structure contains per-service-context state related to the oplog.
 */
class LocalOplogInfo {
public:
    static OplogSlotTimeContext& getOplogSlotTimeContext(OperationContext* opCtx);
    static LocalOplogInfo* get(ServiceContext& service);
    static LocalOplogInfo* get(ServiceContext* service);
    static LocalOplogInfo* get(OperationContext* opCtx);

    LocalOplogInfo(const LocalOplogInfo&) = delete;
    LocalOplogInfo& operator=(const LocalOplogInfo&) = delete;
    LocalOplogInfo() = default;

    RecordStore* getRecordStore() const;
    void setRecordStore(OperationContext* opCtx, RecordStore* rs);
    void resetRecordStore();

    /**
     * Sets the global Timestamp to be 'newTime'.
     */
    void setNewTimestamp(ServiceContext* service, const Timestamp& newTime);

    /**
     * Allocates optimes for new entries in the oplog. Returns the new optimes in a vector along
     * with their terms.
     */
    std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count);

    /**
     * Returns a shared reference to the oplog truncate markers to allow the caller to wait
     * for a deletion request.
     */
    std::shared_ptr<OplogTruncateMarkers> getTruncateMarkers() const;

    /**
     * Sets the truncate markers once the initial markers have been created.
     */
    void setTruncateMarkers(std::shared_ptr<OplogTruncateMarkers> markers);

private:
    // The "oplog" record store pointer is always valid (or null) because an operation must take
    // the global exclusive lock to set the pointer to null when the RecordStore instance is
    // destroyed. See "oplogCheckCloseDatabase".
    RecordStore* _rs = nullptr;

    // Stores truncate markers for this oplog, can be nullptr e.g. when
    // the server is read-only.
    std::shared_ptr<OplogTruncateMarkers> _truncateMarkers;
    // Mutex for concurrent access to above fields
    mutable stdx::mutex _rsMutex;

    // Synchronizes the section where a new Timestamp is generated and when it is registered in the
    // storage engine.
    mutable stdx::mutex _newOpMutex;

    // Tracks timestamp reservations and controls oplog visibility.
    // This will be default-constructed and will not be properly re-initialized if
    // gFeatureFlagOplogVisibility is disabled.
    // TODO SERVER-85788: Update/remove this comment once the feature flag is removed.
    repl::OplogVisibilityManager _oplogVisibilityManager;
};

}  // namespace MONGO_MOD_PUB mongo
