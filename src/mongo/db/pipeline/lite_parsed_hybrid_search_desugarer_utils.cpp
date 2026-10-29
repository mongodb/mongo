// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer_utils.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/lite_parsed_union_with.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo::lite_parsed_hybrid_search_desugarer::common_utils {
using namespace std::literals::string_view_literals;

namespace {

// Throws the invalid-weight-name error via the shared helper so both parse paths throw
// identically.
[[noreturn]] void failWeightsValidationWithPipelineSuggestions(
    const std::vector<std::string>& pipelineNames,
    const StringSet& matchedPipelines,
    const std::vector<std::string>& invalidWeights,
    std::string_view stageName) {
    std::vector<std::string> unmatchedPipelines;
    for (const auto& name : pipelineNames) {
        if (!matchedPipelines.contains(name)) {
            unmatchedPipelines.push_back(name);
        }
    }

    hybrid_scoring_util::failWeightsValidationWithPipelineSuggestions(
        unmatchedPipelines, invalidWeights, stageName);
    MONGO_UNREACHABLE;
}

}  // namespace

StringMap<double> validateWeights(const BSONObj& inputWeights,
                                  const std::vector<std::string>& pipelineNames,
                                  std::string_view stageName) {
    StringSet pipelineNameSet(pipelineNames.begin(), pipelineNames.end());

    StringMap<double> weights;
    std::vector<std::string> invalidWeights;
    StringSet matchedPipelines;

    for (const auto& weightEntry : inputWeights) {
        const auto* fieldName = weightEntry.fieldName();

        if (!pipelineNameSet.contains(fieldName)) {
            invalidWeights.emplace_back(fieldName);
            continue;
        }

        uassert(12559402,
                str::stream() << "A pipeline named '" << fieldName
                              << "' is specified more than once in the " << stageName
                              << "'combinations.weight' object.",
                !weights.contains(fieldName));

        uassert(12559404,
                str::stream() << stageName
                              << "'s pipeline weight must be numeric, but given non-numeric value "
                                 "for pipeline '"
                              << fieldName << "'.",
                weightEntry.isNumber());

        const double weight = weightEntry.Number();
        uassert(12559401,
                str::stream() << stageName << "'s pipeline weight must be non-negative, but given "
                              << weight << " for pipeline '" << fieldName << "'.",
                weight >= 0);

        weights[fieldName] = weight;
        matchedPipelines.insert(fieldName);
    }

    if (static_cast<int>(pipelineNames.size()) < inputWeights.nFields()) {
        uasserted(
            12559403,
            fmt::format(
                "{} input has more weights ({}) than pipelines ({}). "
                "If 'combination.weights' is specified, there must be a less or equal number of "
                "weights as pipelines, each of which is unique and existing. "
                "Possible extraneous specified weights = [{}]",
                stageName,
                inputWeights.nFields(),
                static_cast<int>(pipelineNames.size()),
                fmt::join(invalidWeights, ", ")));
    } else if (!invalidWeights.empty()) {
        failWeightsValidationWithPipelineSuggestions(
            pipelineNames, matchedPipelines, invalidWeights, stageName);
    }

    return weights;
}

BSONObj buildReplaceRootBson(std::string_view docsName) {
    return BSON("$replaceWith" << BSON(docsName << "$$ROOT"));
}

BSONObj buildSortByScoreMetaBson() {
    return BSON("$sort" << BSON("score" << BSON("$meta" << "score") << "_id" << 1));
}

BSONObj buildProjectRemoveInternalFieldsBson(std::string_view internalFieldsName) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"sv));
        projectBob.append(internalFieldsName, 0);
    }
    return bob.obj();
}

BSONObj buildGroupBson(const std::vector<std::string>& pipelineNames,
                       bool includeScoreDetails,
                       std::string_view internalFieldsName,
                       std::string_view docsName,
                       std::string_view detailsScalarSuffix) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder gBob(bob.subobjStart("$group"sv));
        gBob.append("_id", fmt::format("${}._id", docsName));
        gBob.append(docsName, BSON("$first" << fmt::format("${}", docsName)));

        auto accumulateScalar = [&](std::string_view field, const std::string& internalPath) {
            gBob.append(fmt::format("{}{}", kHsFlatFieldPrefix, field),
                        BSON("$max" << BSON("$ifNull"
                                            << BSON_ARRAY(fmt::format("${}", internalPath) << 0))));
        };

        for (const auto& pipelineName : pipelineNames) {
            const std::string scoreField =
                hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName);
            accumulateScalar(scoreField,
                             hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                 internalFieldsName, scoreField));
            if (includeScoreDetails) {
                const std::string detailsScalarField =
                    fmt::format("{}{}", pipelineName, detailsScalarSuffix);
                accumulateScalar(detailsScalarField,
                                 hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                     internalFieldsName, detailsScalarField));

                const std::string scoreDetailsField = fmt::format("{}_scoreDetails", pipelineName);
                const std::string internalScoreDetailsPath =
                    hybrid_scoring_util::applyInternalFieldPrefixToFieldName(internalFieldsName,
                                                                             scoreDetailsField);
                gBob.append(fmt::format("{}{}", kHsFlatFieldPrefix, scoreDetailsField),
                            BSON("$mergeObjects" << fmt::format("${}", internalScoreDetailsPath)));
            }
        }
    }
    return bob.obj();
}

BSONObj buildReplaceRootMergeBson(const std::vector<std::string>& pipelineNames,
                                  bool includeScoreDetails,
                                  std::string_view internalFieldsName,
                                  std::string_view docsName,
                                  std::string_view detailsScalarSuffix) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder rrBob(bob.subobjStart("$replaceRoot"sv));
        BSONObjBuilder newRootBob(rrBob.subobjStart("newRoot"sv));
        BSONArrayBuilder mergeArr(newRootBob.subarrayStart("$mergeObjects"sv));
        mergeArr.append(fmt::format("${}", docsName));
        BSONObjBuilder wrapperBob;
        {
            BSONObjBuilder internalFieldsBob(wrapperBob.subobjStart(internalFieldsName));
            auto appendFlat = [&](std::string_view field) {
                internalFieldsBob.append(field, fmt::format("${}{}", kHsFlatFieldPrefix, field));
            };
            for (const auto& pipelineName : pipelineNames) {
                appendFlat(hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName));
                if (includeScoreDetails) {
                    appendFlat(fmt::format("{}{}", pipelineName, detailsScalarSuffix));
                    appendFlat(fmt::format("{}_scoreDetails", pipelineName));
                }
            }
        }
        mergeArr.append(wrapperBob.obj());
    }
    return bob.obj();
}

std::unique_ptr<LiteParsedDocumentSource> parseOwnedStage(const NamespaceString& nss,
                                                          BSONObj stageBson) {
    auto lpds = LiteParsedDocumentSource::parse(nss, stageBson);
    lpds->makeOwned();
    return lpds;
}

void mutateRightmostSortToOutputSortKey(const NamespaceString& nss, StageSpecs& stages) {
    // No-op if no $sort is present; e.g. $search and $vectorSearch provide sorted output
    // via sort-key metadata without an explicit $sort stage.
    for (auto it = stages.rbegin(); it != stages.rend(); ++it) {
        if ((*it)->getParseTimeName() == "$sort") {
            BSONElement origSpec = (*it)->getOriginalBson();
            uassert(12559415,
                    "Expected $sort spec value to be an object",
                    origSpec.type() == BSONType::object);

            BSONObjBuilder mergedSortSpec;
            mergedSortSpec.appendElements(origSpec.embeddedObject());
            if (!origSpec.embeddedObject().hasField("$_internalOutputSortKeyMetadata"sv)) {
                mergedSortSpec.append("$_internalOutputSortKeyMetadata", true);
            }

            BSONObj newStageBson = BSON("$sort" << mergedSortSpec.obj());
            *it = parseOwnedStage(nss, std::move(newStageBson));
            return;
        }
    }
}

std::unique_ptr<LiteParsedDocumentSource> buildUnionWithLPDS(const NamespaceString& nss,
                                                             std::string_view userCollName,
                                                             StageSpecs perPipelineStages) {
    // Serialize the already-parsed (and sort-mutated) stages back to BSON for the $unionWith's
    // own serialization/explain output. This BSON is best-effort only — it is not used to
    // rebuild the inner pipeline below, since stages whose original BSON is only a placeholder
    // (e.g. AST-only extension sub-stages) cannot round-trip through BSON. Each .wrap() call
    // produces a self-owning copy, so rawPipeline does not alias any stage's internal buffer.
    std::vector<BSONObj> rawPipeline;
    rawPipeline.reserve(perPipelineStages.size());
    for (const auto& s : perPipelineStages) {
        rawPipeline.push_back(s->getOriginalBson().wrap());
    }

    NamespaceString foreignNss = NamespaceStringUtil::deserialize(nss.dbName(), userCollName);

    // Move the already-parsed stages directly into the inner pipeline rather than reparsing the
    // best-effort BSON above.
    OwnedLiteParsedPipeline innerLpp(foreignNss, std::move(perPipelineStages));

    BSONObjBuilder bob;
    {
        BSONObjBuilder uwBob(bob.subobjStart("$unionWith"sv));
        uwBob.append("coll", userCollName);
        BSONArrayBuilder pipeArr(uwBob.subarrayStart("pipeline"sv));
        for (const auto& obj : rawPipeline) {
            pipeArr.append(obj);
        }
    }
    BSONObj unionWithObj = bob.obj();

    auto lpuw = std::make_unique<LiteParsedUnionWith>(
        unionWithObj.firstElement(),
        std::move(foreignNss),
        boost::optional<OwnedLiteParsedPipeline>(std::move(innerLpp)),
        std::move(rawPipeline),
        /*isHybridSearch=*/true);

    // makeOwned() wraps _originalBson (a BSONElement view into unionWithObj) into a shared buffer,
    // keeping the underlying BSON alive after unionWithObj goes out of scope.
    lpuw->makeOwned();
    return lpuw;
}

}  // namespace mongo::lite_parsed_hybrid_search_desugarer::common_utils
