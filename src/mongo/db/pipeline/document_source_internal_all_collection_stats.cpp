// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceInternalAllCollectionStats::DocumentSourceInternalAllCollectionStats(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    DocumentSourceInternalAllCollectionStatsSpec spec)
    : DocumentSource(kStageName, pExpCtx), _internalAllCollectionStatsSpec(std::move(spec)) {}

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalAllCollectionStats,
                                     DocumentSourceInternalAllCollectionStats::LiteParsed::parse,
                                     AllowedWithApiStrict::kInternal);

// Custom registration because this stage uses createFromBsonInternal instead of createFromBson.
DocumentSourceContainer _internalAllCollectionStatsStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<InternalAllCollectionStatsStageParams*>(stageParams.get());
    return {DocumentSourceInternalAllCollectionStats::createFromBsonInternal(
        typedParams->getOriginalBson(), expCtx)};
}

ALLOCATE_AND_REGISTER_STAGE_PARAMS(_internalAllCollectionStats,
                                   InternalAllCollectionStatsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalAllCollectionStats,
                            DocumentSourceInternalAllCollectionStats::id);

DocumentSourceContainer::iterator DocumentSourceInternalAllCollectionStats::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Attempt to internalize any predicates of a $project stage in order to calculate only
    // necessary fields.
    if (auto nextProject =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>((*std::next(itr)).get())) {
        _projectFilter = nextProject->getTransformer().serializeTransformation().toBson();
    }

    // Attempt to internalize any predicates of a $match upon the "ns" field.
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    auto splitMatch = std::move(*nextMatch).splitSourceBy({"ns"}, {});
    invariant(splitMatch.first || splitMatch.second);

    // Remove the original $match.
    container->erase(std::next(itr));

    // Absorb the part of $match that is dependant on 'ns'
    if (splitMatch.second) {
        if (!_absorbedMatch) {
            _absorbedMatch = std::move(splitMatch.second);
        } else {
            // We have already absorbed a $match. We need to join it with splitMatch.second.
            _absorbedMatch->joinMatchWith(std::move(splitMatch.second),
                                          MatchExpression::MatchType::AND);
        }
    }

    // splitMatch.first is independent of 'ns'. Put it back on the pipeline.
    if (splitMatch.first) {
        container->insert(std::next(itr), std::move(splitMatch.first));
        return std::next(itr);
    } else {
        // There may be further optimization between this stage and the new neighbor, so we return
        // an iterator pointing to ourself.
        return itr;
    }
}

void DocumentSourceInternalAllCollectionStats::serializeToArray(
    std::vector<Value>& array, const query_shape::SerializationOptions& opts) const {
    auto explain = opts.verbosity;
    if (explain) {
        BSONObjBuilder bob;
        _internalAllCollectionStatsSpec.serialize(&bob, opts);
        if (_absorbedMatch) {
            bob.append("match", _absorbedMatch->getQuery());
        }
        auto doc = Document{{getSourceName(), bob.obj()}};
        array.push_back(Value(doc));
    } else {
        array.push_back(serialize(opts));
        if (_absorbedMatch) {
            _absorbedMatch->serializeToArray(array);
        }
    }
}

intrusive_ptr<DocumentSource> DocumentSourceInternalAllCollectionStats::createFromBsonInternal(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6789103,
            str::stream() << "$_internalAllCollectionStats must take a nested object but found: "
                          << elem,
            elem.type() == BSONType::object);

    uassert(6789104,
            "The $_internalAllCollectionStats stage must be run on the admin database",
            pExpCtx->getNamespaceString().isAdminDB() &&
                pExpCtx->getNamespaceString().isCollectionlessAggregateNS());

    auto spec = DocumentSourceInternalAllCollectionStatsSpec::parse(elem.embeddedObject(),
                                                                    IDLParserContext(kStageName));

    return make_intrusive<DocumentSourceInternalAllCollectionStats>(pExpCtx, std::move(spec));
}

std::string_view DocumentSourceInternalAllCollectionStats::getSourceName() const {
    return kStageName;
}

Value DocumentSourceInternalAllCollectionStats::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _internalAllCollectionStatsSpec.toBSON(opts)}});
}
}  // namespace mongo
