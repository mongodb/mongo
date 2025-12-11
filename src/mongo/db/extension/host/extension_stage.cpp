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

#include "mongo/db/extension/host/extension_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/adapter/executable_agg_stage_adapter.h"
#include "mongo/db/extension/shared/get_next_result.h"

namespace mongo {

using namespace extension::host;

boost::intrusive_ptr<exec::agg::Stage> documentSourceExtensionToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto* documentSource = dynamic_cast<DocumentSourceExtensionOptimizable*>(source.get());
    tassert(10980400, "expected 'DocumentSourceExtensionOptimizable' type", documentSource);
    auto execAggStageHandle = documentSource->compile();
    return make_intrusive<exec::agg::ExtensionStage>(documentSource->getSourceName(),
                                                     documentSource->getExpCtx(),
                                                     std::move(execAggStageHandle));
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(extensionStage,
                           DocumentSourceExtensionOptimizable::id,
                           documentSourceExtensionToStageFn);

ExtensionStage::ExtensionStage(StringData name,
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
        _execAggStageHandle.setSource(nullptr);
    }

    if (pSource) {
        _sourceAggStageHandle = extension::ExecAggStageHandle{
            new extension::host_connector::HostExecAggStageAdapter(ExecAggStage::make(pSource))};
        // Check if the allocation failed. This might be superfluous, as any allocation failure
        // would have likely oomed/bad_alloc.
        tassert(10957205, "_sourceAggStageHandle is invalid", _sourceAggStageHandle.isValid());
        // Attach the reference on extension side.
        _execAggStageHandle.setSource(_sourceAggStageHandle);
    }
}

GetNextResult ExtensionStage::doGetNext() {
    using namespace mongo::extension;
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(pExpCtx.get());
    host_connector::QueryExecutionContextAdapter ctxAdapter(std::move(wrappedCtx));
    tassert(11357601, "_execAggStageHandle is invalid", _execAggStageHandle.isValid());
    _lastGetNextResult = _execAggStageHandle.getNext(&ctxAdapter);
    switch (_lastGetNextResult.code) {
        case GetNextCode::kAdvanced: {
            tassert(11357602,
                    "No result BSONObj returned even though the result is in the advanced state.",
                    _lastGetNextResult.resultDocument.has_value());
            MutableDocument mutableDoc(
                Document{_lastGetNextResult.resultDocument->getUnownedBSONObj()});
            if (_lastGetNextResult.resultMetadata.has_value()) {
                for (const auto& elem : _lastGetNextResult.resultMetadata->getUnownedBSONObj()) {
                    auto metaType = DocumentMetadataFields::parseMetaTypeFromQualifiedString(
                        elem.fieldNameStringData());
                    mutableDoc.metadata().setMetaFieldFromValue(metaType, Value(elem));
                }
            }
            return GetNextResult(mutableDoc.freeze());
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

Document ExtensionStage::getExplainOutput(const SerializationOptions& opts) const {
    MutableDocument output(Stage::getExplainOutput(opts));

    BSONObj explainSerialization = _execAggStageHandle.explain(*opts.verbosity);
    for (auto elem : explainSerialization) {
        output.addField(elem.fieldName(), Value(elem));
    }

    return output.freeze();
}
}  // namespace exec::agg
}  // namespace mongo
