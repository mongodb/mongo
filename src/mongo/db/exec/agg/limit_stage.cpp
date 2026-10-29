// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/exec/agg/limit_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceLimitToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* dsLimit = dynamic_cast<DocumentSourceLimit*>(documentSource.get());

    tassert(10816500, "expected 'DocumentSourceLimit' type", dsLimit);

    return make_intrusive<exec::agg::LimitStage>(
        dsLimit->kStageName, dsLimit->getExpCtx(), dsLimit->getLimit());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(limitStage, DocumentSourceLimit::id, documentSourceLimitToStageFn)

LimitStage::LimitStage(std::string_view stageName,
                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       long long limit)
    : Stage(stageName, expCtx), _limit(limit) {}

GetNextResult LimitStage::doGetNext() {
    if (_nReturned >= _limit) {
        return GetNextResult::makeEOF();
    }

    auto nextInput = pSource->getNext();
    if (nextInput.isAdvanced()) {
        ++_nReturned;
        if (_nReturned >= _limit) {
            dispose();
        }
    }

    return nextInput;
}

}  // namespace exec::agg
}  // namespace mongo
