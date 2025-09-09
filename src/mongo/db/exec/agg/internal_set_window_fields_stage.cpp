/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/internal_set_window_fields_stage.h"

#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

namespace mongo::exec::agg {

namespace {
MONGO_FAIL_POINT_DEFINE(overrideMemoryLimitForSpill);
}

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSetWindowFieldsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* setWindowFieldsDS =
        dynamic_cast<DocumentSourceInternalSetWindowFields*>(documentSource.get());

    tassert(10423400, "expected 'DocumentSourceInternalSetWindowFields' type", setWindowFieldsDS);

    return make_intrusive<exec::agg::InternalSetWindowFieldsStage>(
        setWindowFieldsDS->kStageName,
        setWindowFieldsDS->getExpCtx(),
        setWindowFieldsDS->getPartitionBy(),
        setWindowFieldsDS->getSortBy(),
        setWindowFieldsDS->getOutputFields());
}

REGISTER_AGG_STAGE_MAPPING(_internalSetWindowFields,
                           DocumentSourceInternalSetWindowFields::id,
                           documentSourceInternalSetWindowFieldsToStageFn);

InternalSetWindowFieldsStage::InternalSetWindowFieldsStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<boost::intrusive_ptr<Expression>>& partitionBy,
    const boost::optional<SortPattern>& sortBy,
    const std::vector<WindowFunctionStatement>& outputFields)
    : Stage(stageName, expCtx),
      _memoryTracker{OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(
          *expCtx,
          expCtx->getAllowDiskUse() && !expCtx->getInRouter(),
          loadMemoryLimit(StageMemoryLimit::DocumentSourceSetWindowFieldsMaxMemoryBytes))},
      // TODO SERVER-98563 I think we can remove the sortBy here in favor of {$meta: "sortKey"}
      // also.
      _iterator(expCtx.get(), pSource, &_memoryTracker, std::move(partitionBy), sortBy),
      _sortBy(sortBy),
      _outputFields(outputFields) {
    initialize();
}

void InternalSetWindowFieldsStage::initialize() {
    for (auto& wfs : _outputFields) {
        _executableOutputs[wfs.fieldName] =
            WindowFunctionExec::create(pExpCtx.get(), &_iterator, wfs, _sortBy, &_memoryTracker);
    }
}

DocumentSource::GetNextResult InternalSetWindowFieldsStage::doGetNext() {
    if (_eof) {
        // On EOF, update SetWindowFieldStats so explain has $_internalSetWindowFields-level
        // statistics.
        _stats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();
        return DocumentSource::GetNextResult::makeEOF();
    }

    auto curDoc = _iterator.current();
    if (!curDoc) {
        if (_iterator.isPaused()) {
            return DocumentSource::GetNextResult::makePauseExecution();
        }
        // The only way we hit this case is if there are no documents, since otherwise _eof will be
        // set.
        _eof = true;
        return DocumentSource::GetNextResult::makeEOF();
    }

    // Populate the output document with the result from each window function.
    auto projSpec = std::make_unique<projection_executor::InclusionNode>(
        ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kIncludeId});
    for (auto&& outputField : _outputFields) {
        try {
            // If we hit a uassert while evaluating expressions on user data, delete the temporary
            // table before aborting the operation.
            auto& fieldName = outputField.fieldName;
            projSpec->addExpressionForPath(
                FieldPath(fieldName),
                ExpressionConstant::create(pExpCtx.get(),
                                           _executableOutputs[fieldName]->getNext(*curDoc)));
        } catch (const DBException&) {
            _iterator.finalize();
            throw;
        }

        bool inMemoryLimit = _memoryTracker.withinMemoryLimit();
        overrideMemoryLimitForSpill.execute([&](const BSONObj& data) {
            _numDocsProcessed++;
            inMemoryLimit = _numDocsProcessed <= data["maxDocsBeforeSpill"].numberInt();
        });

        if (!inMemoryLimit && _memoryTracker.allowDiskUse()) {
            // Attempt to spill where possible.
            _iterator.spillToDisk();
            _stats.spillingStats = _iterator.getSpillingStats();
        }
        if (!_memoryTracker.withinMemoryLimit()) {
            _iterator.finalize();
            uasserted(5414201,
                      str::stream()
                          << "Exceeded memory limit in DocumentSourceSetWindowFields, used "
                          << _memoryTracker.inUseTrackedMemoryBytes()
                          << " bytes but max allowed is "
                          << _memoryTracker.maxAllowedMemoryUsageBytes());
        }
    }

    // Advance the iterator and handle partition/EOF edge cases.
    switch (_iterator.advance()) {
        case PartitionIterator::AdvanceResult::kAdvanced:
            break;
        case PartitionIterator::AdvanceResult::kNewPartition:
            // We've advanced to a new partition, reset the state of every function.
            for (auto&& [fieldName, function] : _executableOutputs) {
                function->reset();
            }
            break;
        case PartitionIterator::AdvanceResult::kEOF:
            _eof = true;
            _iterator.finalize();
            _stats.spillingStats = _iterator.getSpillingStats();
            break;
    }

    // Avoid using the factory 'create' on the executor since we don't want to re-parse.
    auto projExec = std::make_unique<projection_executor::AddFieldsProjectionExecutor>(
        pExpCtx, std::move(projSpec));

    return projExec->applyProjection(*curDoc);
}

void InternalSetWindowFieldsStage::doDispose() {
    // Before we clear the memory tracker, update SetWindowFieldStats so explain has
    // $_internalSetWindowFields-level statistics.
    _stats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();

    _iterator.finalize();
    _stats.spillingStats = _iterator.getSpillingStats();
}

Document InternalSetWindowFieldsStage::getExplainOutput(const SerializationOptions& opts) const {
    MutableDocument out(Stage::getExplainOutput(opts));
    MutableDocument md;

    for (auto&& [fieldName, function] : _executableOutputs) {
        md[opts.serializeFieldPathFromString(fieldName)] = opts.serializeLiteral(
            static_cast<long long>(_memoryTracker.peakTrackedMemoryBytes(fieldName)));
    }

    out["maxFunctionMemoryUsageBytes"] = Value(md.freezeToValue());

    // TODO SERVER-88298 Remove maxTotalMemoryUsageBytes when we enable feature flag as
    // peakTrackedMemBytes reports the same value.
    out["maxTotalMemoryUsageBytes"] =
        opts.serializeLiteral(static_cast<long long>(_memoryTracker.peakTrackedMemoryBytes()));
    out["usedDisk"] = opts.serializeLiteral(_iterator.usedDisk());
    out["spills"] = opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpills()));
    out["spilledDataStorageSize"] = opts.serializeLiteral(
        static_cast<long long>(_stats.spillingStats.getSpilledDataStorageSize()));
    out["spilledBytes"] =
        opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledBytes()));
    out["spilledRecords"] =
        opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledRecords()));
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        out["peakTrackedMemBytes"] =
            opts.serializeLiteral(static_cast<long long>(_stats.peakTrackedMemBytes));
    }

    return out.freeze();
}

}  // namespace mongo::exec::agg
