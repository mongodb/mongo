// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

/**
 * The settings that decide what the reads of an operation see, captured from a "main" operation so
 * that they can be given to the worker OperationContexts which read on its behalf from other
 * threads (e.g. the producers of a parallel query plan).
 *
 * A worker that has been given the view of its main operation reads what the main operation would
 * have read at that moment had it opened a new snapshot itself:
 *  - a point-in-time read (snapshot read concern, atClusterTime) reads at the same timestamp;
 *  - a majority read resolves the majority-committed timestamp again at each of its snapshots;
 *  - a local/available read on a secondary reads at lastApplied, re-derived at each acquisition;
 *  - otherwise the worker reads the latest committed data;
 * and in every case ignores or enforces prepare conflicts as the main operation does, and times out
 * when the main operation does (e.g. while waiting on a prepare conflict). Workers open
 * snapshots independently of each other and of the main operation, so non-point-in-time reads can
 * observe different committed states at the same moment.
 *
 * Not every main operation has a view that can be shared, see capture().
 */
class [[MONGO_MOD_PUBLIC]] SharedReadView {
public:
    /**
     * Captures the read view of 'opCtx'. Fails with IllegalOperation, and the reason, when the
     * reads of 'opCtx' can't be carried out by other OperationContexts given its view.
     */
    static StatusWith<SharedReadView> capture(OperationContext* opCtx);

    /**
     * Gives 'workerOpCtx' the captured view. Must be called before 'workerOpCtx' opens a storage
     * snapshot or acquires any collection.
     */
    void applyTo(OperationContext* workerOpCtx) const;

private:
    SharedReadView() = default;

    repl::ReadConcernArgs _readConcern;
    RecoveryUnit::ReadSource _readSource = RecoveryUnit::ReadSource::kNoTimestamp;
    // Only set for kProvided: every other read source resolves its timestamp from the storage
    // engine each time a snapshot is opened.
    boost::optional<Timestamp> _readTimestamp;
    PrepareConflictBehavior _prepareConflictBehavior = PrepareConflictBehavior::block;
    bool _readOnce = false;
    bool _enforceConstraints = true;
    Date_t _deadline = Date_t::max();
    ErrorCodes::Error _timeoutError = ErrorCodes::ExceededTimeLimit;
};

}  // namespace mongo::sbe
