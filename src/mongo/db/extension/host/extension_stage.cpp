// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/adapter/executable_agg_stage_adapter.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/util/scoped_timer_metric.h"
#include "mongo/db/stats/counters.h"

#include <string_view>

namespace mongo {

using ExecTimeDuration = Microseconds;
auto& totalAggStageExecTime =
    *MetricBuilder<DurationCounter64<ExecTimeDuration>>("extension.totalAggStageExecMicros");

using namespace extension::host;

/**
 * Converts a DocumentSourceExtensionOptimizable (pipeline stage) into an exec::agg::ExtensionStage.
 * Compiles the document source to an ExecAggStageHandle and wraps it in ExtensionStage.
 */
boost::intrusive_ptr<exec::agg::Stage> documentSourceExtensionToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto* documentSource = dynamic_cast<DocumentSourceExtensionOptimizable*>(source.get());
    tassert(10980400, "expected 'DocumentSourceExtensionOptimizable' type", documentSource);

    // Increment extensionVectorSearchQueryCount if this is a $vectorSearch extension stage.
    if (documentSource->getSourceName() == search_helpers::kExtensionVectorSearchStageName) {
        vector_search_metrics::extensionVectorSearchQueryCount.increment(1);
    }

    // Increment extensionSearchQueryCount if this is a $search or $searchMeta extension stage.
    if (documentSource->getSourceName() == search_helpers::kExtensionSearchStageName ||
        documentSource->getSourceName() == search_helpers::kExtensionSearchMetaStageName) {
        search_metrics::extensionSearchQueryCount.increment(1);
    }

    auto execAggStageHandle = documentSource->compile();
    return make_intrusive<exec::agg::ExtensionStage>(documentSource->getSourceName(),
                                                     documentSource->getExpCtx(),
                                                     std::move(execAggStageHandle));
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(extensionStage,
                           DocumentSourceExtensionOptimizable::id,
                           documentSourceExtensionToStageFn);

ExtensionStage::ExtensionStage(std::string_view name,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                               extension::ExecAggStageHandle execAggStageHandle)
    : Stage(name, pExpCtx),
      _execAggStageHandle(std::move(execAggStageHandle)),
      _sourceAggStageHandle(nullptr) {
    tassert(10957211, "_execAggStageHandle is invalid", _execAggStageHandle.isValid());
}

void ExtensionStage::setSource(Stage* source) {
    // First, let's check that we have a valid _execAggStageHandle before we proceed with
    // allocations.
    tassert(10957204, "_execAggStageHandle is invalid", _execAggStageHandle.isValid());

    // This sets pSource to be source.
    Stage::setSource(source);

    // Remove any reference to the pointer in the extension before deleting the old handle.
    if (_sourceAggStageHandle.isValid()) {
        _execAggStageHandle->setSource(nullptr);
    }

    if (pSource) {
        _sourceAggStageHandle = extension::ExecAggStageHandle{
            new extension::host_connector::HostExecAggStageAdapter(ExecAggStage::make(pSource))};
        // Check if the allocation failed. This might be superfluous, as any allocation failure
        // would have likely oomed/bad_alloc.
        tassert(10957205, "_sourceAggStageHandle is invalid", _sourceAggStageHandle.isValid());
        // Attach the reference on extension side.
        _execAggStageHandle->setSource(_sourceAggStageHandle);
    }
}

GetNextResult ExtensionStage::doGetNext() {
    using namespace mongo::extension;
    // Track and report time spent in this method:
    ScopedTimerMetric timer(getContext()->getOperationContext(), totalAggStageExecTime);

    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(pExpCtx.get());
    host_connector::QueryExecutionContextAdapter ctxAdapter(std::move(wrappedCtx),
                                                            _dynamicBatchSize);
    tassert(11357601, "_execAggStageHandle is invalid", _execAggStageHandle.isValid());
    _lastGetNextResult = _execAggStageHandle->getNext(&ctxAdapter);
    switch (_lastGetNextResult.code) {
        case GetNextCode::kAdvanced: {
            tassert(ErrorCodes::ExtensionError,
                    "No result BSONObj returned even though the result is in the advanced state.",
                    _lastGetNextResult.resultDocument.has_value());

            auto nextDocument = [&]() {
                if (_lastGetNextResult.resultMetadata.has_value()) {
                    return Document::createDocumentWithMetadata(
                        _lastGetNextResult.resultDocument->getUnownedBSONObj().getOwned(),
                        _lastGetNextResult.resultMetadata->getUnownedBSONObj().getOwned());
                }
                return Document{_lastGetNextResult.resultDocument->getUnownedBSONObj().getOwned()};
            }();
            return GetNextResult(std::move(nextDocument));
        }
        case GetNextCode::kPauseExecution:
            return GetNextResult::makePauseExecution();
        case GetNextCode::kEOF:
            return GetNextResult::makeEOF();
        default:
            tasserted(11357603,
                      str::stream()
                          << "Invalid GetNextCode: " << static_cast<int>(_lastGetNextResult.code));
    }
}

Document ExtensionStage::getExplainOutput(const query_shape::SerializationOptions& opts) const {
    MutableDocument output(Stage::getExplainOutput(opts));

    std::unique_ptr<extension::host::QueryExecutionContext> wrappedCtx =
        std::make_unique<extension::host::QueryExecutionContext>(pExpCtx.get());
    extension::host_connector::QueryExecutionContextAdapter ctxAdapter(std::move(wrappedCtx));
    BSONObj explainSerialization = _execAggStageHandle->explain(ctxAdapter, *opts.verbosity);
    for (auto elem : explainSerialization) {
        output.addField(elem.fieldName(), Value(elem));
    }

    return output.freeze();
}
}  // namespace exec::agg
}  // namespace mongo
