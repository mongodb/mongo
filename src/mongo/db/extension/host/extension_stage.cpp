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
    : Stage(name, pExpCtx), _execAggStageHandle(std::move(execAggStageHandle)) {
    tassert(11357600, "_execAggStageHandle is invalid", _execAggStageHandle.isValid());
}

GetNextResult ExtensionStage::doGetNext() {
    if (pSource) {
        // TODO (SERVER-112713): Call the api's set_source instead of returning pSource->getNext()
        // so that pSource can be passed as an input stage to transform extension stages.
        return pSource->getNext();
    }
    using namespace mongo::extension;
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(pExpCtx.get());
    host_connector::QueryExecutionContextAdapter ctxAdapter(std::move(wrappedCtx));
    tassert(11357601, "execAggStageHandle is invalid", _execAggStageHandle.isValid());
    auto result = _execAggStageHandle.getNext(&ctxAdapter);
    switch (result.code) {
        case GetNextCode::kAdvanced: {
            tassert(11357602,
                    "No result BSONObj returned even though the result is in the advanced state.",
                    result.res.has_value());
            return GetNextResult(Document{result.res.get()});
        }
        case GetNextCode::kPauseExecution:
            return GetNextResult::makePauseExecution();
        case GetNextCode::kEOF:
            return GetNextResult::makeEOF();
        default:
            tasserted(11357603,
                      str::stream()
                          << "Invalid GetNextCode: " << static_cast<const int>(result.code));
    }
}
}  // namespace exec::agg
}  // namespace mongo
