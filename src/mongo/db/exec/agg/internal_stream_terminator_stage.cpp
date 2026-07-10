/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
