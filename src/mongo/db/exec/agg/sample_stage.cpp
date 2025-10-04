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

#include "mongo/db/exec/agg/sample_stage.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/sort_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"

#include <cstdint>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> sampleStageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSourceSample) {
    auto* ptr = dynamic_cast<DocumentSourceSample*>(documentSourceSample.get());
    tassert(10980800, "expected 'DocumentSourceSample' type", ptr);
    return make_intrusive<exec::agg::SampleStage>(
        ptr->kStageName, ptr->getExpCtx(), ptr->getSampleSize());
}

namespace {
const BSONObj randSortSpec = BSON("$rand" << BSON("$meta" << "randVal"));
}  // namespace

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(sampleStage, DocumentSourceSample::id, sampleStageToStageFn);

SampleStage::SampleStage(StringData stageName,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         long long size)
    : Stage(stageName, pExpCtx), _size(size) {
    _sortStage = boost::dynamic_pointer_cast<SortStage>(buildStage(DocumentSourceSort::create(
        pExpCtx,
        SortPattern(randSortSpec, pExpCtx),
        DocumentSourceSort::SortStageOptions{.limit = static_cast<uint64_t>(_size)})));
}

GetNextResult SampleStage::doGetNext() {
    if (_size == 0) {
        pSource->dispose();
        return GetNextResult::makeEOF();
    }

    if (!_sortStage->isPopulated()) {
        // Exhaust source stage, add random metadata, and push all into sorter.
        PseudoRandom& prng = pExpCtx->getOperationContext()->getClient()->getPrng();
        auto nextInput = pSource->getNext();
        for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
            MutableDocument doc(nextInput.releaseDocument());
            doc.metadata().setRandVal(prng.nextCanonicalDouble());
            _sortStage->loadDocument(doc.freeze());
        }
        switch (nextInput.getStatus()) {
            case GetNextResult::ReturnStatus::kAdvanced: {
                MONGO_UNREACHABLE;  // We consumed all advances above.
            }
            case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
                tasserted(10358901, "Sample does not support control events");
            }
            case GetNextResult::ReturnStatus::kPauseExecution: {
                return nextInput;  // Propagate the pause.
            }
            case GetNextResult::ReturnStatus::kEOF: {
                _sortStage->loadingDone();
            }
        }
    }

    invariant(_sortStage->isPopulated());
    return _sortStage->getNext();
}
}  // namespace exec::agg
}  // namespace mongo
