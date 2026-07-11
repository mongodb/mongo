// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_set_window_fields_stage.h"

#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

#include <string_view>

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
    std::string_view stageName,
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

    auto projSpec = std::make_unique<projection_executor::InclusionNode>(
        ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kIncludeId});
    _constExprs.reserve(_outputFields.size());
    _orderedExecs.reserve(_outputFields.size());
    for (auto&& outputField : _outputFields) {
        auto constExpr = ExpressionConstant::create(pExpCtx.get(), Value{});
        _constExprs.push_back(constExpr);
        _orderedExecs.push_back(_executableOutputs[outputField.fieldName].get());
        projSpec->addExpressionForPath(FieldPath(outputField.fieldName), std::move(constExpr));
    }
    _projExec = std::make_unique<projection_executor::AddFieldsProjectionExecutor>(
        pExpCtx, std::move(projSpec));
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

    tassert(12369900,
            "_constExprs and _outputFields must have the same size",
            _constExprs.size() == _outputFields.size() &&
                _orderedExecs.size() == _outputFields.size());

    // Populate the output document with the result from each window function.
    for (size_t i = 0; i < _outputFields.size(); ++i) {
        try {
            // If we hit a uassert while evaluating expressions on user data, delete the temporary
            // table before aborting the operation.
            _constExprs[i]->setValue(_orderedExecs[i]->getNext(*curDoc));
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

    return _projExec->applyProjection(*curDoc, {});
}

void InternalSetWindowFieldsStage::doDispose() {
    // Before we clear the memory tracker, update SetWindowFieldStats so explain has
    // $_internalSetWindowFields-level statistics.
    _stats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();

    _iterator.finalize();
    _stats.spillingStats = _iterator.getSpillingStats();
}

Document InternalSetWindowFieldsStage::getExplainOutput(
    const query_shape::SerializationOptions& opts) const {
    MutableDocument out(Stage::getExplainOutput(opts));
    MutableDocument md;

    for (auto&& [fieldName, function] : _executableOutputs) {
        md[opts.serializeFieldPathFromString(fieldName)] = opts.serializeLiteral(
            static_cast<long long>(_memoryTracker.peakTrackedMemoryBytes(fieldName)));
    }

    out["maxFunctionMemoryUsageBytes"] = Value(md.freezeToValue());
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
    } else {
        out["maxMemoryUsageBytes"] =
            opts.serializeLiteral(static_cast<long long>(_memoryTracker.peakTrackedMemoryBytes()));
    }

    return out.freeze();
}

}  // namespace mongo::exec::agg
