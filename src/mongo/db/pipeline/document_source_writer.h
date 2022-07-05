/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <fmt/format.h>

#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {
using namespace fmt::literals;

/**
 * Manipulates the state of the OperationContext so that while this object is in scope, reads and
 * writes will use a local read concern and see the latest version of the data. It will also reset
 * ignore_prepared on the recovery unit so that any reads or writes will block on a conflict with a
 * prepared transaction. Resets the OperationContext back to its original state upon destruction.
 */
class DocumentSourceWriteBlock {
    OperationContext* _opCtx;
    repl::ReadConcernArgs _originalArgs;
    RecoveryUnit::ReadSource _originalSource;
    EnforcePrepareConflictsBlock _enforcePrepareConflictsBlock;
    Timestamp _originalTimestamp;

public:
    DocumentSourceWriteBlock(OperationContext* opCtx)
        : _opCtx(opCtx), _enforcePrepareConflictsBlock(opCtx) {
        _originalArgs = repl::ReadConcernArgs::get(_opCtx);
        _originalSource = _opCtx->recoveryUnit()->getTimestampReadSource();
        if (_originalSource == RecoveryUnit::ReadSource::kProvided) {
            // Storage engine operations require at least Global IS.
            Lock::GlobalLock lk(_opCtx, MODE_IS);
            _originalTimestamp = *_opCtx->recoveryUnit()->getPointInTimeReadTimestamp(_opCtx);
        }

        repl::ReadConcernArgs::get(_opCtx) = repl::ReadConcernArgs();
        _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    }

    ~DocumentSourceWriteBlock() {
        repl::ReadConcernArgs::get(_opCtx) = _originalArgs;
        if (_originalSource == RecoveryUnit::ReadSource::kProvided) {
            _opCtx->recoveryUnit()->setTimestampReadSource(_originalSource, _originalTimestamp);
        } else {
            _opCtx->recoveryUnit()->setTimestampReadSource(_originalSource);
        }
    }
};

/**
 * This is a base abstract class for all stages performing a write operation into an output
 * collection. The writes are organized in batches in which elements are objects of the templated
 * type 'B'. A subclass must override two methods to be able to write into the output collection:
 *
 *    1. 'makeBatchObject()' - to create an object of type 'B' from the given 'Document', which is,
 *       essentially, a result of the input source's 'getNext()' .
 *    2. 'spill()' - to write the batch into the output collection.
 *
 * Two other virtual methods exist which a subclass may override: 'initialize()' and 'finalize()',
 * which are called before the first element is read from the input source, and after the last one
 * has been read, respectively.
 */
template <typename B>
class DocumentSourceWriter : public DocumentSource {
public:
    using BatchObject = B;
    using BatchedObjects = std::vector<BatchObject>;

    DocumentSourceWriter(const char* stageName,
                         NamespaceString outputNs,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(stageName, expCtx),
          _outputNs(std::move(outputNs)),
          _writeConcern(expCtx->opCtx->getWriteConcern()) {}

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        deps->needWholeDocument = true;
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

    GetModPathsReturn getModifiedPaths() const override {
        // For purposes of tracking which fields come from where, the writer stage does not modify
        // any fields by default.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const override {
        return true;
    }

    const NamespaceString& getOutputNs() const {
        return _outputNs;
    }

protected:
    GetNextResult doGetNext() final override;
    /**
     * Prepares the stage to be able to write incoming batches.
     */
    virtual void initialize() {}

    /**
     * Finalize the output collection, called when there are no more documents to write.
     */
    virtual void finalize() {}

    /**
     * Writes the documents in 'batch' to the output namespace.
     */
    virtual void spill(BatchedObjects&& batch) = 0;

    /**
     * Creates a batch object from the given document and returns it to the caller along with the
     * object size.
     */
    virtual std::pair<B, int> makeBatchObject(Document&& doc) const = 0;

    /**
     * A subclass may override this method to enable a fail point right after a next input element
     * has been retrieved, but not processed yet.
     */
    virtual void waitWhileFailPointEnabled() {}

    // The namespace where the output will be written to.
    const NamespaceString _outputNs;

    // Stash the writeConcern of the original command as the operation context may change by the
    // time we start to spill writes. This is because certain aggregations (e.g. $exchange)
    // establish cursors with batchSize 0 then run subsequent getMore's which use a new operation
    // context. The getMore's will not have an attached writeConcern however we still want to
    // respect the writeConcern of the original command.
    WriteConcernOptions _writeConcern;

private:
    bool _initialized{false};
    bool _done{false};
};

template <typename B>
DocumentSource::GetNextResult DocumentSourceWriter<B>::doGetNext() {
    if (_done) {
        return GetNextResult::makeEOF();
    }

    // Ignore writes and exhaust input if we are in explain mode.
    if (pExpCtx->explain) {
        auto nextInput = pSource->getNext();
        for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        }
        _done = nextInput.getStatus() == GetNextResult::ReturnStatus::kEOF;
        return nextInput;
    } else {
        // Ensure that the client's operationTime reflects the latest write even if the command
        // fails.
        ON_BLOCK_EXIT(
            [&] { pExpCtx->mongoProcessInterface->updateClientOperationTime(pExpCtx->opCtx); });

        if (!_initialized) {
            initialize();
            _initialized = true;
        }

        BatchedObjects batch;
        int bufferedBytes = 0;

        auto nextInput = pSource->getNext();
        for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
            waitWhileFailPointEnabled();

            auto doc = nextInput.releaseDocument();
            auto [obj, objSize] = makeBatchObject(std::move(doc));

            bufferedBytes += objSize;
            if (!batch.empty() &&
                (bufferedBytes > BSONObjMaxUserSize ||
                 batch.size() >= write_ops::kMaxWriteBatchSize)) {
                spill(std::move(batch));
                batch.clear();
                bufferedBytes = objSize;
            }
            batch.push_back(obj);
        }
        if (!batch.empty()) {
            spill(std::move(batch));
            batch.clear();
        }

        switch (nextInput.getStatus()) {
            case GetNextResult::ReturnStatus::kAdvanced: {
                MONGO_UNREACHABLE;  // We consumed all advances above.
            }
            case GetNextResult::ReturnStatus::kPauseExecution: {
                return nextInput;  // Propagate the pause.
            }
            case GetNextResult::ReturnStatus::kEOF: {
                _done = true;
                finalize();
                return nextInput;
            }
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
