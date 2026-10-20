// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/validate/validate_adaptor.h"
#include "mongo/util/cancellation.h"

#include <memory>

namespace mongo {

class ThreadPool;

namespace collection_validation {

/**
 * Returns a shared thread pool for validation runs.
 */
std::shared_ptr<ThreadPool> getValidateThreadPool();

/**
 * Reads through a cursor of its own, unthrottled, so that it can run on a worker thread alongside
 * the other slices of a parallel scan. The parallel path is restricted to foreground validation,
 * where throttling is off regardless, so nothing is lost by bypassing the throttle.
 *
 * It does still yield. A slice that holds one cursor positioned for the whole of its range never
 * releases the storage engine's cursor, and the storage engine only asks a thread to help with
 * cache eviction when that thread has no cursor open. An unyielding slice therefore scans the
 * collection at full speed while ordinary operations elsewhere are the ones stalled to evict the
 * pages the slice is bringing in. Resetting the cursor periodically puts the worker back under the
 * same back-pressure as everyone else.
 *
 * Not final so that tests can substitute the records it reads.
 */
class SliceValidateCursor : public ValidateCursor {
public:
    SliceValidateCursor(OperationContext* opCtx,
                        std::unique_ptr<SeekableRecordCursor> cursor,
                        CancellationToken cancellation);

    boost::optional<Record> seek(const RecordId& recordId) override;
    boost::optional<Record> next() override;
    void yield() override;

private:
    OperationContext* _opCtx;
    std::unique_ptr<SeekableRecordCursor> _cursor;
    CancellationToken _cancellation;
};

class ValidateAdaptorParallel : public ValidateAdaptor {
public:
    ValidateAdaptorParallel(OperationContext* opCtx,
                            collection_validation::ValidateState& validateState,
                            boost::optional<int64_t> targetRecordsPerSlice);

    void traverseRecordStore(OperationContext* opCtx,
                             ValidateResults& validateResults,
                             ValidationVersion validationVersion) final;

protected:
    /**
     * Called once the validation owns the shared thread pool, before it schedules any slice.
     * Overridden by tests.
     */
    virtual void onThreadPoolAcquired_forTest() const {}

    /**
     * Opens the cursor that one slice reads through. Called on the slice's worker thread, with the
     * worker's OperationContext. 'cancellation' is how an interrupt of the validation reaches the
     * slice. Overridden by tests.
     */
    virtual std::unique_ptr<ValidateCursor> makeSliceCursor(OperationContext* workerOpCtx,
                                                            const RecordStore& recordStore,
                                                            CancellationToken cancellation) const;

private:
    boost::optional<int64_t> _targetRecordsPerSlice;
};

}  // namespace collection_validation
}  // namespace mongo
