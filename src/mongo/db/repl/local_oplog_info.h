// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_visibility_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/oplog_truncate_markers.h"
#include "mongo/db/storage/storage_oplog_manager.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <mutex>
#include <vector>

#include <boost/optional/optional.hpp>


namespace [[MONGO_MOD_PUBLIC]] mongo {

// Stores the total time an operation spends with an uncommitted oplog slot held open. Indicator
// that an operation is holding back replication by causing oplog holes to remain open for
// unusual amounts of time.
class OplogSlotTimeContext {
    int64_t _batchCount = 0;
    boost::optional<Timer> _timer;
    Atomic<int64_t> _totalOplogSlotDurationMicros;

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
     *
     * The opTimeOffset is an increment applied to the base opTime when registering the oplog
     * visibility point, allowing the caller to move the visible, hole-free end of the oplog forward
     * by a configurable amount.
     */
    std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx,
                                          std::size_t count,
                                          std::size_t opTimeOffset = 0);

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
    mutable std::mutex _rsMutex;

    // Synchronizes the section where a new Timestamp is generated and when it is registered in the
    // storage engine.
    mutable std::mutex _newOpMutex;

    // Tracks timestamp reservations and controls oplog visibility.
    // This will be default-constructed and will not be properly re-initialized if
    // gFeatureFlagOplogVisibility is disabled.
    // TODO SERVER-85788: Update/remove this comment once the feature flag is removed.
    repl::OplogVisibilityManager _oplogVisibilityManager;

    StorageOplogManager* _oplogManager = nullptr;
};

}  // namespace mongo
