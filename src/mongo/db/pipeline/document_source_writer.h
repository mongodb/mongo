// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {

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
        _originalSource = shard_role_details::getRecoveryUnit(_opCtx)->getTimestampReadSource();
        if (_originalSource == RecoveryUnit::ReadSource::kProvided) {
            // Storage engine operations require at least Global IS.
            Lock::GlobalLock lk(_opCtx, MODE_IS);
            _originalTimestamp =
                *shard_role_details::getRecoveryUnit(_opCtx)->getPointInTimeReadTimestamp();
        }

        repl::ReadConcernArgs::get(_opCtx) = repl::ReadConcernArgs();
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kNoTimestamp);
    }

    ~DocumentSourceWriteBlock() {
        repl::ReadConcernArgs::get(_opCtx) = _originalArgs;
        if (_originalSource == RecoveryUnit::ReadSource::kProvided) {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalSource,
                                                                                _originalTimestamp);
        } else {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalSource);
        }
    }
};

/**
 * This is a base abstract class for all DocumentSources performing a write
 * operation into an output collection.
 */
class DocumentSourceWriter : public DocumentSource {
public:
    DocumentSourceWriter(std::string_view stageName,
                         NamespaceString outputNs,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(stageName, expCtx), _outputNs(std::move(outputNs)) {}

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        deps->needWholeDocument = true;
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

    GetModPathsReturn getModifiedPaths() const override {
        // For purposes of tracking which fields come from where, the writer stage does not modify
        // any fields by default.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const override {
        return true;
    }

    const NamespaceString& getOutputNs() const {
        return _outputNs;
    }

    boost::optional<ShardId> computeMergeShardId() const final {
        return getExpCtx()->getMongoProcessInterface()->determineSpecificMergeShard(
            getExpCtx()->getOperationContext(), getOutputNs());
    }

private:
    const NamespaceString _outputNs;
};


}  // namespace mongo
