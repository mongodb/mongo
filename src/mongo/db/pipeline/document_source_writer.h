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

#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/storage/recovery_unit.h"

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
    DocumentSourceWriter(const char* stageName,
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

    boost::optional<ShardId> computeMergeShardId() const final {
        return getExpCtx()->getMongoProcessInterface()->determineSpecificMergeShard(
            getExpCtx()->getOperationContext(), getOutputNs());
    }

private:
    const NamespaceString _outputNs;
};


}  // namespace mongo
