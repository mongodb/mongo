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
#include "mongo/db/exec/agg/skip_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSkipToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* dsSkip = dynamic_cast<DocumentSourceSkip*>(documentSource.get());

    tassert(10816501, "expected 'DocumentSourceSkip' type", dsSkip);

    return make_intrusive<exec::agg::SkipStage>(
        dsSkip->kStageName, dsSkip->getExpCtx(), dsSkip->getSkip());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(skipStage, DocumentSourceSkip::id, documentSourceSkipToStageFn)

SkipStage::SkipStage(StringData stageName,
                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     long long nToSkip)
    : Stage(stageName, expCtx), _nToSkip(nToSkip) {}

GetNextResult SkipStage::doGetNext() {
    while (_nSkippedSoFar < _nToSkip) {
        // For performance reasons, a streaming stage must not keep references to documents across
        // calls to getNext(). Such stages must retrieve a result from their child and then release
        // it (or return it) before asking for another result. Failing to do so can result in extra
        // work, since the Document/Value library must copy data on write when that data has a
        // refcount above one.
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }
        ++_nSkippedSoFar;
    }

    return pSource->getNext();
}

}  // namespace exec::agg
}  // namespace mongo
