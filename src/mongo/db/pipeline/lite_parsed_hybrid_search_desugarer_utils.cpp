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

#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer_utils.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/lite_parsed_union_with.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/query/util/string_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo::lite_parsed_hybrid_search_desugarer::common_utils {

namespace {

// Builds and throws the "invalid weight name" error with typo suggestions.
[[noreturn]] void failWeightsValidationWithPipelineSuggestions(
    const std::vector<std::string>& pipelineNames,
    const StringSet& matchedPipelines,
    const std::vector<std::string>& invalidWeights,
    StringData stageName) {
    std::vector<std::string> unmatchedPipelines;
    for (const auto& name : pipelineNames) {
        if (!matchedPipelines.contains(name)) {
            unmatchedPipelines.push_back(name);
        }
    }

    std::vector<std::pair<std::string, std::vector<std::string>>> suggestions =
        query_string_util::computeTypoSuggestions(unmatchedPipelines, invalidWeights);

    auto convertSingleSuggestionToString = [&](std::size_t i) -> std::string {
        std::string s = fmt::format("(provided: '{}' -> ", suggestions[i].first);
        if (suggestions[i].second.size() == 1) {
            s += fmt::format("suggested: '{}')", suggestions[i].second.front());
        } else {
            s += fmt::format("suggestions: [{}])", fmt::join(suggestions[i].second, ", "));
        }
        if (i < suggestions.size() - 1) {
            s += ", ";
        }
        return s;
    };

    std::string errorMsg = fmt::format(
        "${} stage contained ({}) weight(s) in "
        "'combination.weights' that did not reference valid pipeline names. "
        "Suggestions for valid pipeline names: ",
        stageName,
        std::to_string(invalidWeights.size()));
    for (std::size_t i = 0; i < suggestions.size(); ++i) {
        errorMsg += convertSingleSuggestionToString(i);
    }

    uasserted(12559400, errorMsg);
}

}  // namespace

StringMap<double> validateWeights(const BSONObj& inputWeights,
                                  const std::vector<std::string>& pipelineNames,
                                  StringData stageName) {
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

BSONObj buildReplaceRootBson(StringData docsName) {
    return BSON("$replaceWith" << BSON(docsName << "$$ROOT"));
}

BSONObj buildSortByScoreMetaBson() {
    return BSON("$sort" << BSON("score" << BSON("$meta" << "score") << "_id" << 1));
}

BSONObj buildProjectRemoveInternalFieldsBson(StringData internalFieldsName) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"_sd));
        projectBob.append(internalFieldsName, 0);
    }
    return bob.obj();
}

BSONObj buildGroupBson(const std::vector<std::string>& pipelineNames,
                       bool includeScoreDetails,
                       StringData internalFieldsName,
                       StringData docsName,
                       StringData detailsScalarSuffix) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder gBob(bob.subobjStart("$group"_sd));
        gBob.append("_id", fmt::format("${}._id", docsName));
        gBob.append(docsName, BSON("$first" << fmt::format("${}", docsName)));

        auto accumulateScalar = [&](StringData field, const std::string& internalPath) {
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
                                  StringData internalFieldsName,
                                  StringData docsName,
                                  StringData detailsScalarSuffix) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder rrBob(bob.subobjStart("$replaceRoot"_sd));
        BSONObjBuilder newRootBob(rrBob.subobjStart("newRoot"_sd));
        BSONArrayBuilder mergeArr(newRootBob.subarrayStart("$mergeObjects"_sd));
        mergeArr.append(fmt::format("${}", docsName));
        BSONObjBuilder wrapperBob;
        {
            BSONObjBuilder internalFieldsBob(wrapperBob.subobjStart(internalFieldsName));
            auto appendFlat = [&](StringData field) {
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
            tassert(12559415,
                    "Expected $sort spec value to be an object",
                    origSpec.type() == BSONType::object);

            BSONObjBuilder mergedSortSpec;
            mergedSortSpec.appendElements(origSpec.embeddedObject());
            if (!origSpec.embeddedObject().hasField("$_internalOutputSortKeyMetadata"_sd)) {
                mergedSortSpec.append("$_internalOutputSortKeyMetadata", true);
            }

            BSONObj newStageBson = BSON("$sort" << mergedSortSpec.obj());
            *it = parseOwnedStage(nss, std::move(newStageBson));
            return;
        }
    }
}

std::unique_ptr<LiteParsedDocumentSource> buildUnionWithLPDS(const NamespaceString& nss,
                                                             StringData userCollName,
                                                             StageSpecs perPipelineStages) {
    // Serialize the already-parsed (and sort-mutated) stages back to BSON. Each .wrap() call
    // produces a self-owning copy, so rawPipeline does not alias any stage's internal buffer.
    std::vector<BSONObj> rawPipeline;
    rawPipeline.reserve(perPipelineStages.size());
    for (const auto& s : perPipelineStages) {
        rawPipeline.push_back(s->getOriginalBson().wrap());
    }

    NamespaceString foreignNss = NamespaceStringUtil::deserialize(nss.dbName(), userCollName);

    // OwnedLiteParsedPipeline owns its backing BSON, so no manual setOwnedBson is needed for the
    // inner pipeline.
    OwnedLiteParsedPipeline innerLpp(foreignNss, rawPipeline);

    BSONObjBuilder bob;
    {
        BSONObjBuilder uwBob(bob.subobjStart("$unionWith"_sd));
        uwBob.append("coll", userCollName);
        BSONArrayBuilder pipeArr(uwBob.subarrayStart("pipeline"_sd));
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
        /*hasForeignDB=*/false,  // views are flattened before this point
        /*isHybridSearch=*/true);

    // makeOwned() wraps _originalBson (a BSONElement view into unionWithObj) into a shared buffer,
    // keeping the underlying BSON alive after unionWithObj goes out of scope.
    lpuw->makeOwned();
    return lpuw;
}

}  // namespace mongo::lite_parsed_hybrid_search_desugarer::common_utils
