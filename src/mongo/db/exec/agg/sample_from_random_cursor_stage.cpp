// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/sample_from_random_cursor_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include <boost/math/distributions/beta.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/math/special_functions/detail/erf_inv.hpp"
// IWYU pragma: no_include "boost/math/special_functions/detail/lanczos_sse2.hpp"

#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
/**
 * Select a random value drawn according to the distribution Beta(alpha=1, beta=N). The kth smallest
 * value of a sample of size N from a Uniform(0, 1) distribution has a Beta(k, N + 1 - k)
 * distribution, so the return value represents the smallest value from such a sample. This is also
 * the expected distance between the values drawn from a uniform distribution, which is how it is
 * being used here.
 */
double smallestFromSampleOfUniform(PseudoRandom* prng, size_t N) {
    boost::math::beta_distribution<double> betaDist(1.0, static_cast<double>(N));
    double p = prng->nextCanonicalDouble();
    return boost::math::quantile(betaDist, p);
}
}  // namespace

boost::intrusive_ptr<exec::agg::Stage> sampleFromRandomCursorStageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSourceSampleFromRandomCursor) {
    auto* ptr = dynamic_cast<DocumentSourceSampleFromRandomCursor*>(
        documentSourceSampleFromRandomCursor.get());
    tassert(10817100, "expected 'DocumentSourceSampleFromRandomCursor' type", ptr);
    return make_intrusive<exec::agg::SampleFromRandomCursorStage>(
        ptr->kStageName,
        ptr->getExpCtx(),
        /* size */ ptr->_size,
        /* idField */ ptr->_idField,
        /* nDocsInCollection */ ptr->_nDocsInColl);
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(sampleFromRandomCursorStage,
                           DocumentSourceSampleFromRandomCursor::id,
                           sampleFromRandomCursorStageToStageFn);

SampleFromRandomCursorStage::SampleFromRandomCursorStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    long long size,
    std::string idField,
    long long nDocsInCollection)
    : Stage(stageName, pExpCtx),
      _size(size),
      _idField(std::move(idField)),
      _seenDocs(pExpCtx->getValueComparator().makeFlatUnorderedValueSet()),
      _nDocsInColl(nDocsInCollection) {}

GetNextResult SampleFromRandomCursorStage::doGetNext() {
    if (_seenDocs.size() >= static_cast<size_t>(_size))
        return GetNextResult::makeEOF();

    auto nextResult = getNextNonDuplicateDocument();
    if (!nextResult.isAdvanced()) {
        return nextResult;
    }

    // Assign it a random value to enable merging by random value, attempting to avoid bias in that
    // process.
    auto& prng = pExpCtx->getOperationContext()->getClient()->getPrng();
    _randMetaFieldVal -= smallestFromSampleOfUniform(&prng, _nDocsInColl);

    MutableDocument md(nextResult.releaseDocument());
    md.metadata().setRandVal(_randMetaFieldVal);
    if (pExpCtx->getNeedsMerge()) {
        // This stage will be merged by sorting results according to this random metadata field, but
        // the merging logic expects to sort by the sort key metadata.
        const bool isSingleElementKey = true;
        md.metadata().setSortKey(Value(_randMetaFieldVal), isSingleElementKey);
    }
    return md.freeze();
}

GetNextResult SampleFromRandomCursorStage::getNextNonDuplicateDocument() {
    // We may get duplicate documents back from the random cursor, and should not return duplicate
    // documents, so keep trying until we get a new one.
    const int kMaxAttempts = 100;
    for (int i = 0; i < kMaxAttempts; ++i) {
        auto nextInput = pSource->getNext();
        switch (nextInput.getStatus()) {
            case GetNextResult::ReturnStatus::kAdvanced: {
                auto idField = nextInput.getDocument()[std::string_view{_idField}];
                uassert(28793,
                        str::stream()
                            << "The optimized $sample stage requires all documents have a "
                            << _idField
                            << " field in order to de-duplicate results, but encountered a "
                               "document without a "
                            << _idField << " field: " << nextInput.getDocument().toString(),
                        !idField.missing());

                if (_seenDocs.insert(std::move(idField)).second) {
                    return nextInput;
                }
                LOGV2_DEBUG(20903,
                            1,
                            "$sample encountered duplicate document: {nextInput_getDocument}",
                            "nextInput_getDocument"_attr =
                                redact(nextInput.getDocument().toString()));
                break;  // Try again with the next document.
            }
            case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
                tasserted(10358902, "Sample from random cursor does not support control events");
            }
            case GetNextResult::ReturnStatus::kPauseExecution: {
                MONGO_UNREACHABLE;  // Our input should be a random cursor, which should never
                                    // result in kPauseExecution.
            }
            case GetNextResult::ReturnStatus::kEOF: {
                return nextInput;
            }
        }
    }
    uasserted(28799,
              str::stream() << "$sample stage could not find a non-duplicate document after "
                            << kMaxAttempts
                            << " while using a random cursor. This is likely a "
                               "sporadic failure, please try again.");
}
}  // namespace exec::agg
}  // namespace mongo
