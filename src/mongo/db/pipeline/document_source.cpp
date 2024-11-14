/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <absl/meta/type_traits.h>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/initializer.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/duration.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using Parser = DocumentSource::Parser;
using boost::intrusive_ptr;
using std::list;
using std::string;
using std::vector;

DocumentSource::DocumentSource(const StringData stageName,
                               const intrusive_ptr<ExpressionContext>& pCtx)
    : pSource(nullptr), pExpCtx(pCtx), _commonStats(stageName.rawData()) {
    if (pExpCtx->shouldCollectDocumentSourceExecStats()) {
        if (internalMeasureQueryExecutionTimeInNanoseconds.load()) {
            _commonStats.executionTime.precision = QueryExecTimerPrecision::kNanos;
        } else {
            _commonStats.executionTime.precision = QueryExecTimerPrecision::kMillis;
        }
    }
}

namespace {
struct ParserRegistration {
    Parser parser;
    boost::optional<FeatureFlag> featureFlag;
};
// Used to keep track of which DocumentSources are registered under which name.
static StringMap<ParserRegistration> parserMap;
}  // namespace

void accumulatePipelinePlanSummaryStats(const Pipeline& pipeline,
                                        PlanSummaryStats& planSummaryStats) {
    auto visitor = PlanSummaryStatsVisitor(planSummaryStats);
    for (auto&& source : pipeline.getSources()) {
        if (auto specificStats = source->getSpecificStats()) {
            specificStats->acceptVisitor(&visitor);
        }
    }
}

void DocumentSource::registerParser(string name,
                                    Parser parser,
                                    boost::optional<FeatureFlag> featureFlag) {
    auto it = parserMap.find(name);
    massert(28707,
            str::stream() << "Duplicate document source (" << name << ") registered.",
            it == parserMap.end());
    parserMap[name] = {parser, featureFlag};
}
void DocumentSource::registerParser(string name,
                                    SimpleParser simpleParser,
                                    boost::optional<FeatureFlag> featureFlag) {

    Parser parser =
        [simpleParser = std::move(simpleParser)](
            BSONElement stageSpec,
            const intrusive_ptr<ExpressionContext>& expCtx) -> list<intrusive_ptr<DocumentSource>> {
        return {simpleParser(std::move(stageSpec), expCtx)};
    };
    return registerParser(std::move(name), std::move(parser), std::move(featureFlag));
}
bool DocumentSource::hasQuery() const {
    return false;
}

BSONObj DocumentSource::getQuery() const {
    MONGO_UNREACHABLE;
}

list<intrusive_ptr<DocumentSource>> DocumentSource::parse(
    const intrusive_ptr<ExpressionContext>& expCtx, BSONObj stageObj) {
    uassert(16435,
            "A pipeline stage specification object must contain exactly one field.",
            stageObj.nFields() == 1);
    BSONElement stageSpec = stageObj.firstElement();
    auto stageName = stageSpec.fieldNameStringData();

    // Get the registered parser and call that.
    auto it = parserMap.find(stageName);

    uassert(16436,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    auto& entry = it->second;
    expCtx->throwIfFeatureFlagIsNotEnabledOnFCV(stageName, entry.featureFlag);

    return it->second.parser(stageSpec, expCtx);
}

intrusive_ptr<DocumentSource> DocumentSource::optimize() {
    return this;
}

namespace {

/**
 * Verifies whether or not a $group is able to swap with a succeeding $match stage. While ordinarily
 * $group can swap with a $match, it cannot if the following $match has exactly one field as the
 * $group key and either:
 *     (1) an $exists predicate on _id
 *     (2) a $type predicate on _id
 *
 * For $exists, every document will have an _id field following such a $group stage, including those
 * whose group key was missing before the $group. As an example, the following optimization would be
 * incorrect as the post-optimization pipeline would handle documents that had nullish _id fields
 * differently. Thus, given such a $group and $match, this function would return false.
 *   {$group: {_id: "$x"}}
 *   {$match: {_id: {$exists: true}}
 * ---->
 *   {$match: {x: {$exists: true}}
 *   {$group: {_id: "$x"}}

 * For $type, the $type operator can distinguish between values that compare equal in the $group
 * stage, meaning documents that are regarded unequally in the $match stage are equated in the
 * $group stage. This leads to varied results depending on the order of the $match and $group.
 */
bool groupMatchSwapVerified(const DocumentSourceMatch& nextMatch,
                            const DocumentSourceGroup& thisGroup) {
    if (thisGroup.getIdFields().size() != 1) {
        return true;
    }
    return !expression::hasExistenceOrTypePredicateOnPath(*(nextMatch.getMatchExpression()),
                                                          "_id"_sd);
}

/**
 * Returns 'true' if the given stage is an internal change stream stage that can appear in a router
 * (mongoS) pipeline, or 'false' otherwise.
 */
bool isChangeStreamRouterPipelineStage(StringData stageName) {
    return change_stream_constants::kChangeStreamRouterPipelineStages.contains(stageName);
}
}  // namespace

bool DocumentSource::pushMatchBefore(Pipeline::SourceContainer::iterator itr,
                                     Pipeline::SourceContainer* container) {
    if (!constraints().canSwapWithMatch) {
        return false;
    }

    auto nextStageAsMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());
    if (!nextStageAsMatch || nextStageAsMatch->isTextQuery()) {
        // We do not need to attempt this optimization if the $match contains a text search
        // predicate because, in that scenario, $match is already required to be the first stage in
        // the pipeline.
        return false;
    }

    // At this point:
    // 1) The next stage after 'this' is $match.
    // 2) The $match stage does not contain a text search predicate.

    // TODO SERVER-55492: Remove the following workaround when there are rename checks for 'other'
    // match expressions.
    if (isChangeStreamRouterPipelineStage(this->getSourceName())) {
        // Always move the $match stage ahead of internal change stream stages appearing in the
        // router (mongoS) pipeline, because they do not access or modify any paths in the input
        // document.
        container->splice(itr, *container, std::next(itr));
        return true;
    }

    auto thisStageAsGroup = dynamic_cast<DocumentSourceGroup*>(this);
    if (thisStageAsGroup && !groupMatchSwapVerified(*nextStageAsMatch, *thisStageAsGroup)) {
        return false;
    }

    auto [renameableMatchPart, nonRenameableMatchPart] =
        DocumentSourceMatch::splitMatchByModifiedFields(nextStageAsMatch, getModifiedPaths());
    invariant(renameableMatchPart || nonRenameableMatchPart);
    if (!renameableMatchPart) {
        return false;
    }

    LOGV2_DEBUG(5943503,
                5,
                "Swapping all or part of a $match stage in front of another stage: ",
                "matchMovingBefore"_attr = redact(renameableMatchPart->serializeToBSONForDebug()),
                "thisStage"_attr = redact(serializeToBSONForDebug()),
                "matchLeftAfter"_attr = redact(
                    nonRenameableMatchPart ? nonRenameableMatchPart->serializeToBSONForDebug()
                                           : BSONObj()));

    // At this point we know that at least part of the $match expression can be moved ahead of
    // 'this'. So, we erase the original $match and move that renameable part ahead of 'this' stage.
    container->erase(std::next(itr));
    container->insert(itr, std::move(renameableMatchPart));

    // If 'nonRenameableMatchPart' is not null, the 'renameableMatchPart' of the $match expression
    // was only one component of the original $match. So, we need to create a new $match stage for
    // the remaining 'nonRenameableMatchPart' and insert it after 'this' - effectively keeping it in
    // its original position in the pipeline.
    if (nonRenameableMatchPart) {
        container->insert(std::next(itr), std::move(nonRenameableMatchPart));
    }
    return true;
}

bool DocumentSource::pushSampleBefore(Pipeline::SourceContainer::iterator itr,
                                      Pipeline::SourceContainer* container) {
    auto nextSample = dynamic_cast<DocumentSourceSample*>((*std::next(itr)).get());
    if (constraints().canSwapWithSkippingOrLimitingStage && nextSample) {

        container->insert(itr, std::move(nextSample));
        container->erase(std::next(itr));

        return true;
    }
    return false;
}

BSONObj DocumentSource::serializeToBSONForDebug() const {
    std::vector<Value> serialized;
    auto opts = SerializationOptions{
        .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
    serializeToArray(serialized, opts);
    if (serialized.empty()) {
        LOGV2_DEBUG(5943501,
                    5,
                    "warning: stage did not serialize to anything as it was trying to be printed "
                    "for debugging");
        return BSONObj();
    }
    if (serialized.size() > 1) {
        LOGV2_DEBUG(5943502, 5, "stage serialized to multiple stages. Ignoring all but the first");
    }
    return serialized[0].getDocument().toBson();
}

bool DocumentSource::pushSingleDocumentTransformOrRedactBefore(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    if (constraints().canSwapWithSingleDocTransformOrRedact) {
        auto nextItr = std::next(itr);
        if (dynamic_cast<DocumentSourceSingleDocumentTransformation*>(nextItr->get()) ||
            dynamic_cast<DocumentSourceRedact*>(nextItr->get())) {
            LOGV2_DEBUG(5943500,
                        5,
                        "Pushing a single document transform stage or a redact stage in ahead of "
                        "the current stage: ",
                        "singleDocTransformOrRedactStage"_attr =
                            redact((*nextItr)->serializeToBSONForDebug()),
                        "currentStage"_attr = redact(serializeToBSONForDebug()));

            // Swap 'itr' and 'nextItr' list nodes.
            container->splice(itr, *container, nextItr);
            return true;
        }
    }
    return false;
}

Pipeline::SourceContainer::iterator DocumentSource::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    // Attempt to swap 'itr' with a subsequent stage, if applicable.
    if (attemptToPushStageBefore(itr, container)) {
        // The stage before the pushed before stage may be able to optimize further, if there is
        // such a stage.
        return std::prev(itr) == container->begin() ? std::prev(itr) : std::prev(std::prev(itr));
    }

    return doOptimizeAt(itr, container);
}

void DocumentSource::serializeToArray(vector<Value>& array,
                                      const SerializationOptions& opts) const {
    Value entry = serialize(opts);
    if (!entry.missing()) {
        array.push_back(entry);
    }
}

namespace {
std::list<boost::intrusive_ptr<DocumentSource>> throwOnParse(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uasserted(6047400, spec.fieldNameStringData() + " stage is only allowed on MongoDB Atlas");
}
std::unique_ptr<LiteParsedDocumentSource> throwOnParseLite(NamespaceString nss,
                                                           const BSONElement& spec) {
    uasserted(6047401, spec.fieldNameStringData() + " stage is only allowed on MongoDB Atlas");
}
}  // namespace
MONGO_INITIALIZER_GROUP(BeginDocumentSourceRegistration,
                        ("default"),
                        ("EndDocumentSourceRegistration"))
MONGO_INITIALIZER_GROUP(EndDocumentSourceRegistration, ("BeginDocumentSourceRegistration"), ())
}  // namespace mongo
