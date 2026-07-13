// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_stream_terminator_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_internal_stream_terminator.h"

#include <string_view>

namespace mongo::exec::agg {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalStreamTerminatorToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    const auto* streamTerminatorDS =
        dynamic_cast<const DocumentSourceInternalStreamTerminator*>(documentSource.get());

    tassert(13096200, "expected 'DocumentSourceInternalStreamTerminator' type", streamTerminatorDS);

    return make_intrusive<exec::agg::InternalStreamTerminatorStage>(
        DocumentSourceInternalStreamTerminator::kStageName, streamTerminatorDS->getExpCtx());
}

REGISTER_AGG_STAGE_MAPPING(_internalStreamTerminator,
                           DocumentSourceInternalStreamTerminator::id,
                           documentSourceInternalStreamTerminatorToStageFn);

InternalStreamTerminatorStage::InternalStreamTerminatorStage(
    std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Stage(stageName, expCtx) {}

GetNextResult InternalStreamTerminatorStage::doGetNext() {
    if (_terminated) {
        return GetNextResult::makeEOF();
    }
    auto next = pSource->getNext();
    if (next.isEOF()) {
        // Source must emit {_eos: true} before natural EOF, missing sentinel results in deadlock.
        tassert(13096201, "Source reached natural EOF without EOS sentinel.", _terminated);
        return GetNextResult::makeEOF();
    }
    const auto& eosVal = next.getDocument()[DocumentSourceInternalStreamTerminator::kEosFieldName];
    if (eosVal.getType() == BSONType::boolean && eosVal.getBool()) {
        _terminated = true;
        dispose();
        return GetNextResult::makeEOF();
    }
    return next;
}

}  // namespace mongo::exec::agg
