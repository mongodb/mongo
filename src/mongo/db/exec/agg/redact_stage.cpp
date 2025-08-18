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

#include "mongo/db/exec/agg/redact_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

boost::intrusive_ptr<exec::agg::Stage> documentSourceRedactToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* redactDS = dynamic_cast<DocumentSourceRedact*>(documentSource.get());

    tassert(10816600, "expected 'DocumentSourceRedact' type", redactDS);

    return make_intrusive<exec::agg::RedactStage>(
        redactDS->kStageName, redactDS->getExpCtx(), redactDS->getRedactProcessor());
}

REGISTER_AGG_STAGE_MAPPING(redact, DocumentSourceRedact::id, documentSourceRedactToStageFn);

RedactStage::RedactStage(StringData stageName,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const std::shared_ptr<RedactProcessor>& redactProcessor)
    : Stage(stageName, expCtx), _redactProcessor{redactProcessor} {}

GetNextResult RedactStage::doGetNext() {
    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        if (boost::optional<Document> result =
                _redactProcessor->process(nextInput.releaseDocument())) {
            return std::move(*result);
        }
    }
    return nextInput;
}

}  // namespace mongo::exec::agg
