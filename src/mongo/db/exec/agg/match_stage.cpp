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

#include "mongo/db/exec/agg/match_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_match.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceMatchToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto matchDS = boost::dynamic_pointer_cast<DocumentSourceMatch>(documentSource);

    tassert(10422700, "expected 'DocumentSourceMatch' type", matchDS);

    return make_intrusive<exec::agg::MatchStage>(
        matchDS->kStageName, matchDS->getExpCtx(), matchDS->_matchProcessor, matchDS->_isTextQuery);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(match, DocumentSourceMatch::id, documentSourceMatchToStageFn)

MatchStage::MatchStage(StringData stageName,
                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                       const std::shared_ptr<MatchProcessor>& matchProcessor,
                       bool isTextQuery)
    : Stage(stageName, pExpCtx), _matchProcessor(matchProcessor), _isTextQuery(isTextQuery) {}

GetNextResult MatchStage::doGetNext() {
    // The user facing error should have been generated earlier.
    massert(17309, "Should never call getNext on a $match stage with $text clause", !_isTextQuery);

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        if (_matchProcessor->process(nextInput.getDocument())) {
            return nextInput;
        }

        // For performance reasons, a streaming stage must not keep references to documents
        // across calls to getNext(). Such stages must retrieve a result from their child and
        // then release it (or return it) before asking for another result. Failing to do so can
        // result in extra work, since the Document/Value library must copy data on write when
        // that data has a refcount above one.
        nextInput.releaseDocument();
    }

    return nextInput;
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
