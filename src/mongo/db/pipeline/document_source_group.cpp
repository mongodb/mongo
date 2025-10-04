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

#include "mongo/db/pipeline/document_source_group.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {

constexpr StringData DocumentSourceGroup::kStageName;

REGISTER_DOCUMENT_SOURCE(group,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGroup::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(group, DocumentSourceGroup::id)

const char* DocumentSourceGroup::getSourceName() const {
    return kStageName.data();
}

boost::intrusive_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    std::vector<AccumulationStatement> accumulationStatements,
    bool willBeMerged,
    boost::optional<int64_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceGroup> groupStage =
        new DocumentSourceGroup(expCtx, maxMemoryUsageBytes);
    groupStage->_groupProcessor->setIdExpression(groupByExpression);
    groupStage->_groupProcessor->setWillBeMerged(willBeMerged);
    for (auto&& statement : accumulationStatements) {
        groupStage->_groupProcessor->addAccumulationStatement(statement);
    }

    return groupStage;
}

DocumentSourceGroup::DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         boost::optional<int64_t> maxMemoryUsageBytes)
    : DocumentSourceGroupBase(kStageName, expCtx, maxMemoryUsageBytes) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return createFromBsonWithMaxMemoryUsage(std::move(elem), expCtx, boost::none);
}

DocumentSourceContainer::iterator DocumentSourceGroup::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (pushDotRenamedMatch(itr, container)) {
        return itr;
    }

    if (tryToGenerateCommonSortKey(itr, container)) {
        return itr;
    }

    if (tryToOptimizeAccN(itr, container)) {
        return itr;
    }

    return std::next(itr);
}

bool DocumentSourceGroup::pushDotRenamedMatch(DocumentSourceContainer::iterator itr,
                                              DocumentSourceContainer* container) {
    if (std::next(itr) == container->end() || std::next(std::next(itr)) == container->end()) {
        return false;
    }

    // Keep separate iterators for each stage (projection, match).
    auto prospectiveProjectionItr = std::next(itr);
    auto prospectiveProjection =
        dynamic_cast<DocumentSourceSingleDocumentTransformation*>(prospectiveProjectionItr->get());

    auto prospectiveMatchItr = std::next(std::next(itr));
    auto prospectiveMatch = dynamic_cast<DocumentSourceMatch*>(prospectiveMatchItr->get());

    if (!prospectiveProjection || !prospectiveMatch) {
        return false;
    }

    StringSet groupingFields;
    for (auto& idFieldsItr : getIdFields()) {
        groupingFields.insert(idFieldsItr.first);
    }

    GetModPathsReturn paths = prospectiveProjection->getModifiedPaths();
    for (const auto& thisComplexRename : paths.complexRenames) {
        // Check if the dotted renaming is done on a grouping field.
        // This ensures that the top level is flat i.e., no arrays.
        if (groupingFields.contains(thisComplexRename.second)) {
            paths.renames[thisComplexRename.first] = thisComplexRename.second;
        }
    }

    // Perform all changes on a copy of the match source.
    boost::intrusive_ptr<DocumentSource> currentMatchCopyDocument =
        prospectiveMatch->clone(prospectiveMatch->getExpCtx());

    auto currentMatchCopyDocumentMatch =
        dynamic_cast<DocumentSourceMatch*>(currentMatchCopyDocument.get());

    // Translate predicate statements based on the projection renames.
    auto matchSplitForProject = currentMatchCopyDocumentMatch->splitMatchByModifiedFields(
        currentMatchCopyDocumentMatch, paths);

    if (matchSplitForProject.first) {
        // Perform the swap of the projection and the match stages.
        container->erase(prospectiveMatchItr);
        container->insert(prospectiveProjectionItr, std::move(matchSplitForProject.first));

        if (matchSplitForProject.second) {
            // If there is a portion of the match stage predicate that is conflicting with the
            // projection, re-insert it below the projection stage.
            container->insert(std::next(prospectiveProjectionItr),
                              std::move(matchSplitForProject.second));
        }

        return true;
    }

    return false;
}

namespace {
template <TopBottomSense sense, bool single = true>
AccumulationStatement makeAccStmtForTopBottom(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                              const SortPattern& sortPattern,
                                              StringData fieldName,
                                              boost::intrusive_ptr<Expression> origExpr) {
    static_assert(
        single,
        "Neither $topN nor $bottomN are supported yet, kFieldNameN must be added to support them");

    // To comply with any internal parsing logic for $top and $bottom accumulators, we need to
    // compose a BSON object that represents the accumulator statement and then parse it.
    BSONObjBuilder bob;
    {
        // This block opens {"fieldName": {...}}.
        BSONObjBuilder accStmtObjBuilder(bob.subobjStart(fieldName));
        {
            // This block opens {"$top": {...}} or {"$bottom": {...}}. Converts $first to $top and
            // $last to $bottom.
            BSONObjBuilder accArgsBuilder(
                accStmtObjBuilder.subobjStart(AccumulatorTopBottomN<sense, single>::kName));

            // {"$top": {"sortBy": ...}}
            // The sort pattern for $top or $bottom accumulators is same as the sort pattern of the
            // sort stage that is being absorbed.
            accArgsBuilder.append(AccumulatorN::kFieldNameSortBy,
                                  sortPattern.serialize({}).toBson());

            // {"$top": {"sortBy": ..., "output": ...}}
            // The output expression of the new $top or $bottom accumulator is same as the
            // expression for $first and $last accumulators.
            origExpr->serialize().addToBsonObj(&accArgsBuilder, AccumulatorN::kFieldNameOutput);

            accArgsBuilder.doneFast();
        }
        accStmtObjBuilder.doneFast();
    }
    auto accStmtObj = bob.done();

    return AccumulationStatement::parseAccumulationStatement(
        pExpCtx.get(), accStmtObj[fieldName], pExpCtx->variablesParseState);
}
}  // namespace

bool DocumentSourceGroup::tryToAbsorbTopKSort(DocumentSourceSort* prospectiveSort,
                                              DocumentSourceContainer::iterator prospectiveSortItr,
                                              DocumentSourceContainer* container) {
    invariant(prospectiveSort);

    // If the $sort has a limit, we cannot absorb it into the $group since we know the selected
    // documents for $limit for sure after all the input are processed.
    if (prospectiveSort->getLimit()) {
        return false;
    }

    auto sortPattern = prospectiveSort->getSortKeyPattern();
    // Does not support sort by meta field(s).
    for (auto&& sortPatternPart : sortPattern) {
        if (sortPatternPart.expression) {
            return false;
        }
    }

    // Collects all $first and $last accumulators. Does not support either $firstN or $lastN
    // accumulators yet.
    auto& accumulators = _groupProcessor->getMutableAccumulationStatements();
    std::vector<size_t> firstLastAccumulatorIndices;
    for (size_t i = 0; i < accumulators.size(); ++i) {
        if (accumulators[i].expr.name == AccumulatorFirst::kName ||
            accumulators[i].expr.name == AccumulatorLast::kName) {
            firstLastAccumulatorIndices.push_back(i);
        } else if (!accumulators[i].expr.factory()->isCommutative()) {
            // If there are any accumulators that are not commutative (i.e. which depend on the
            // input order) we cannot absorb the $sort into $group because they rely on the ordered
            // input from $sort.
            return false;
        }
    }

    // There's nothing to optimize.
    if (firstLastAccumulatorIndices.empty()) {
        return false;
    }

    for (auto i : firstLastAccumulatorIndices) {
        if (accumulators[i].expr.name == AccumulatorFirst::kName) {
            accumulators[i] = makeAccStmtForTopBottom<TopBottomSense::kTop>(
                getExpCtx(), sortPattern, accumulators[i].fieldName, accumulators[i].expr.argument);
        } else if (accumulators[i].expr.name == AccumulatorLast::kName) {
            accumulators[i] = makeAccStmtForTopBottom<TopBottomSense::kBottom>(
                getExpCtx(), sortPattern, accumulators[i].fieldName, accumulators[i].expr.argument);
        }
    }

    container->erase(prospectiveSortItr);

    return true;
}

namespace {
// The key to group $top(N)/$bottom(N) with the same sort pattern and the same N into a hash table.
struct TopBottomAccKey {
    SortPattern sortPattern;
    AccumulatorN::AccumulatorType accType;
    Value n;
};

// Hasher for 'TopBottomAccKey'.
struct Hasher {
    Hasher(const ValueComparator& comparator) : hash(&comparator) {}
    uint64_t operator()(const TopBottomAccKey& key) const {
        uint64_t h1 = std::hash<AccumulatorN::AccumulatorType>()(key.accType);
        uint64_t h2 = std::hash<std::string>()(key.sortPattern.serialize({}).toString());
        uint64_t h3 = static_cast<uint64_t>(hash(key.n));
        return (h1 ^ h2) ^ h3;
    }

    ValueComparator::Hasher hash;
};

// Equality comparer for 'TopBottomAccKey'.
struct EqualTo {
    EqualTo(const ValueComparator& comparator) : eq(&comparator) {}
    bool operator()(const TopBottomAccKey& lhs, const TopBottomAccKey& rhs) const {
        return lhs.accType == rhs.accType && lhs.sortPattern == rhs.sortPattern && eq(lhs.n, rhs.n);
    }

    ValueComparator::EqualTo eq;
};

// Indices for grouped accumulators into the vector of 'AccumuationStatement'.
using AccIndices = absl::InlinedVector<size_t, 4>;

// Hash table to group $top(N)/$bottom(N) with the same sort pattern.
using TopBottomAccKeyToAccIndicesMap =
    absl::flat_hash_map<TopBottomAccKey, AccIndices, Hasher, EqualTo>;

TopBottomAccKey getTopBottomAccKey(AccumulatorN* accN) {
    switch (accN->getAccumulatorType()) {
        case AccumulatorN::kTop:
            return {.sortPattern = getAccSortPattern<TopBottomSense::kTop, true>(accN),
                    .accType = AccumulatorN::kTop,
                    .n = Value{1}};
        case AccumulatorN::kTopN:
            return {.sortPattern = getAccSortPattern<TopBottomSense::kTop, false>(accN),
                    .accType = AccumulatorN::kTopN,
                    .n = Value(0)};
        case AccumulatorN::kBottom:
            return {.sortPattern = getAccSortPattern<TopBottomSense::kBottom, true>(accN),
                    .accType = AccumulatorN::kBottom,
                    .n = Value(1)};
        case AccumulatorN::kBottomN:
            return {.sortPattern = getAccSortPattern<TopBottomSense::kBottom, false>(accN),
                    .accType = AccumulatorN::kBottomN,
                    .n = Value(0)};
        default:
            MONGO_UNREACHABLE;
    }
}

template <TopBottomSense sense, bool single>
constexpr StringData getMergeFieldNameForAcc() {
    if constexpr (sense == TopBottomSense::kTop && single) {
        return "ts"_sd;
    } else if constexpr (sense == TopBottomSense::kTop && !single) {
        return "tns"_sd;
    } else if constexpr (sense == TopBottomSense::kBottom && single) {
        return "bs"_sd;
    } else if constexpr (sense == TopBottomSense::kBottom && !single) {
        return "bns"_sd;
    }
};

boost::intrusive_ptr<Expression> getOutputArgExpr(boost::intrusive_ptr<Expression> argExpr) {
    auto exprObj = dynamic_cast<ExpressionObject*>(argExpr.get());
    tassert(8808700, "Expected object-type expression", exprObj);
    auto&& exprs = exprObj->getChildExpressions();
    auto outputArgExprIt = std::find_if(exprs.begin(), exprs.end(), [&](auto expr) {
        return expr.first == AccumulatorN::kFieldNameOutput;
    });
    tassert(8808701,
            fmt::format("'{}' field not found", AccumulatorN::kFieldNameOutput),
            outputArgExprIt != exprs.end());
    return outputArgExprIt->second;
};

template <TopBottomSense sense, bool single>
AccumulationStatement mergeAccStmtFor(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                      const std::vector<AccumulationStatement>& accStmts,
                                      Value n,
                                      const SortPattern& sortPattern,
                                      const AccIndices& accIndices,
                                      BSONObjBuilder& prjArgsBuilder) {
    constexpr auto prefix = getMergeFieldNameForAcc<sense, single>();

    // In order to account for multiple instances of the same accumulator (e.g. $topN), we
    // incorporate the first index of an accumulator that will be merged into the field name.
    std::stringstream ss;
    ss << prefix << "_" << accIndices[0];
    const auto mergeFieldName = ss.str();

    // To comply with any internal parsing logic for $top and $bottom accumulators, we need to
    // compose a BSON object that represents the accumulator statement and then parse it.
    BSONObjBuilder bob;
    {
        // This block opens {"tops": {...}}.
        BSONObjBuilder accStmtObjBuilder(bob.subobjStart(mergeFieldName));
        {
            // This block opens {"$top(N)": {...}} or {"$bottom(N)": {...}}.
            BSONObjBuilder accArgsBuilder(
                accStmtObjBuilder.subobjStart(AccumulatorTopBottomN<sense, single>::kName));

            // {"$topN": {"n": ...}}
            if (!single) {
                n.addToBsonObj(&accArgsBuilder, AccumulatorN::kFieldNameN);
            }

            // {"$topN": {"n": ..., "sortBy": ...}}
            accArgsBuilder.append(AccumulatorN::kFieldNameSortBy,
                                  sortPattern.serialize({}).toBson());
            {
                // This block opens "output": {...} inside {"$top": {...}}
                BSONObjBuilder outputBuilder(
                    accArgsBuilder.subobjStart(AccumulatorN::kFieldNameOutput));
                for (auto accIdx : accIndices) {
                    {
                        // This block opens "fieldName": {...} within "output": {...}
                        BSONObjBuilder ifNullOutputBuilder(
                            outputBuilder.subobjStart(accStmts[accIdx].fieldName));
                        {
                            // Composes {$ifNull: ["outputExpression", null]}.
                            BSONArrayBuilder ifNullArrayBuilder(
                                ifNullOutputBuilder.subarrayStart("$ifNull"_sd));
                            getOutputArgExpr(accStmts[accIdx].expr.argument)
                                ->serialize()
                                .addToBsonArray(&ifNullArrayBuilder);
                            ifNullArrayBuilder.appendNull();
                        }
                    }

                    // Recomputes the rewritten nested accumulator fields to the user-requested
                    // fields.
                    prjArgsBuilder.append(
                        accStmts[accIdx].fieldName,
                        fmt::format("${}.{}", mergeFieldName, accStmts[accIdx].fieldName));
                }
                outputBuilder.doneFast();
            }
            accArgsBuilder.doneFast();
        }
        accStmtObjBuilder.doneFast();
    }
    auto accStmtObj = bob.done();

    return AccumulationStatement::parseAccumulationStatement(
        pExpCtx.get(), accStmtObj[mergeFieldName], pExpCtx->variablesParseState);
}

AccumulationStatement makeAccStmtForFirstLast(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                              AccumulatorFirstLastN::Sense sense,
                                              StringData fieldName,
                                              boost::intrusive_ptr<Expression> origExpr) {
    const auto accName =
        sense == AccumulatorFirstLastN::kFirst ? AccumulatorFirst::kName : AccumulatorLast::kName;

    BSONObjBuilder bob;
    {
        // This block opens {"fieldName": {...}}.
        BSONObjBuilder accStmtObjBuilder(bob.subobjStart(fieldName));
        {
            // This block opens {"accName": origExpr}
            origExpr->serialize().addToBsonObj(&accStmtObjBuilder, accName);
        }
        accStmtObjBuilder.doneFast();
    }
    auto accStmtObj = bob.done();

    return AccumulationStatement::parseAccumulationStatement(
        pExpCtx.get(), accStmtObj[fieldName], pExpCtx->variablesParseState);
}

using Sense = std::variant<TopBottomSense, AccumulatorFirstLastN::Sense>;
using AccConversionFunction = std::function<AccumulationStatement(AccumulationStatement)>;

struct AccumulatorInfo {
    bool multi;
    Sense sense;
};

// Contains info about all allowed accumulators for the tryToOptimizeAccN() optimization. The info
// is useful to determine whether accumulators can be converted (i.e. are `multi`), and to compare
// senses of different accumulators.
const std::map<StringData, AccumulatorInfo> kAccNameToInfoMap{
    {AccumulatorFirst::kName, {false, AccumulatorFirstLastN::kFirst}},
    {AccumulatorLast::kName, {false, AccumulatorFirstLastN::kLast}},
    {AccumulatorFirstN::kName, {true, AccumulatorFirstLastN::kFirst}},
    {AccumulatorLastN::kName, {true, AccumulatorFirstLastN::kLast}},
    {AccumulatorTop::kName, {false, TopBottomSense::kTop}},
    {AccumulatorBottom::kName, {false, TopBottomSense::kBottom}},
    {AccumulatorTopN::kName, {true, TopBottomSense::kTop}},
    {AccumulatorBottomN::kName, {true, TopBottomSense::kBottom}}};

AccConversionFunction createAccConversionFunction(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                                  Sense sense,
                                                  boost::optional<SortPattern> sortPattern) {
    return visit(
        OverloadedVisitor{
            [&](const AccumulatorFirstLastN::Sense firstLastSense) -> AccConversionFunction {
                return [=](AccumulationStatement acc) {
                    return makeAccStmtForFirstLast(
                        pExpCtx, firstLastSense, acc.fieldName, acc.expr.argument);
                };
            },
            [&, sortPattern](const TopBottomSense topBottomSense) -> AccConversionFunction {
                if (topBottomSense == TopBottomSense::kTop) {
                    return [=](AccumulationStatement acc) {
                        return makeAccStmtForTopBottom<TopBottomSense::kTop>(
                            pExpCtx,
                            *sortPattern,
                            acc.fieldName,
                            getOutputArgExpr(acc.expr.argument));
                    };
                } else {
                    return [=](AccumulationStatement acc) {
                        return makeAccStmtForTopBottom<TopBottomSense::kBottom>(
                            pExpCtx,
                            *sortPattern,
                            acc.fieldName,
                            getOutputArgExpr(acc.expr.argument));
                    };
                }
            }},
        sense);
}
}  // namespace

bool DocumentSourceGroup::tryToGenerateCommonSortKey(DocumentSourceContainer::iterator itr,
                                                     DocumentSourceContainer* container) {
    auto& accStmts = getMutableAccumulationStatements();

    TopBottomAccKeyToAccIndicesMap topBottomAccKeyToAccIndicesMap(
        0, Hasher(getExpCtx()->getValueComparator()), EqualTo(getExpCtx()->getValueComparator()));
    std::vector<size_t> ineligibleAccIndices;
    bool foundDupSortPattern = false;
    for (size_t accIdx = 0; accIdx < accStmts.size(); ++accIdx) {
        if (accStmts[accIdx].expr.name != AccumulatorTop::kName &&
            accStmts[accIdx].expr.name != AccumulatorBottom::kName &&
            accStmts[accIdx].expr.name != AccumulatorTopN::kName &&
            accStmts[accIdx].expr.name != AccumulatorBottomN::kName) {
            ineligibleAccIndices.push_back(accIdx);
            continue;
        }

        // Composes the key (the sort pattern + acc type) to group the same top or bottom with the
        // same sort pattern. Unfortunately, the sort pattern can be extracted only from
        // 'AccumulatorN' object at this point and so we need to create one using the factory.
        auto accN = accStmts[accIdx].expr.factory();
        auto key = getTopBottomAccKey(dynamic_cast<AccumulatorN*>(accN.get()));
        if (key.accType == AccumulatorN::AccumulatorType::kTopN ||
            key.accType == AccumulatorN::AccumulatorType::kBottomN) {
            key.n = accStmts[accIdx].expr.initializer->serialize();
        }

        if (auto [it, inserted] =
                topBottomAccKeyToAccIndicesMap.try_emplace(std::move(key), AccIndices{accIdx});
            !inserted) {
            it->second.push_back(accIdx);
            foundDupSortPattern = true;
        }
    }

    // Bails out early if we didn't find any duplicated sort pattern for the same accumulator type.
    if (!foundDupSortPattern) {
        return false;
    }

    // Moves over non-eligible accumulator statements to the new accumulators.
    // Also prepares a $project stage to recompute the rewritten nested accumulator fields to the
    // user-requested fields like {$project: {tm: "$ts.tm"}. Note that unoptimized fields should be
    // included as well in the $project spec.
    std::vector<AccumulationStatement> newAccStmts;
    BSONObjBuilder prjArgsBuilder;
    for (auto ineligibleAccIdx : ineligibleAccIndices) {
        prjArgsBuilder.append(accStmts[ineligibleAccIdx].fieldName, 1);
        newAccStmts.push_back(std::move(accStmts[ineligibleAccIdx]));
    }

    for (auto&& [key, accIndices] : topBottomAccKeyToAccIndicesMap) {
        // This accumulator is eligible for the optimization but there's only single accumulator
        // statement that uses the sort pattern with the same accumulator type.
        if (accIndices.size() < 2) {
            auto accIdx = accIndices[0];
            prjArgsBuilder.append(accStmts[accIdx].fieldName, 1);
            newAccStmts.push_back(std::move(accStmts[accIdx]));
            continue;
        }

        // There are multiple accumulator statements that use the same sort pattern with the same
        // accumulator type. We can optimize these accumulators so that they generate the sort key
        // only once at run-time.
        auto mergedAccStmt = [&, &key = key, &accIndices = accIndices] {
            switch (key.accType) {
                case AccumulatorN::AccumulatorType::kTop:
                    return mergeAccStmtFor<TopBottomSense::kTop, true>(
                        getExpCtx(), accStmts, key.n, key.sortPattern, accIndices, prjArgsBuilder);
                case AccumulatorN::AccumulatorType::kTopN:
                    return mergeAccStmtFor<TopBottomSense::kTop, false>(
                        getExpCtx(), accStmts, key.n, key.sortPattern, accIndices, prjArgsBuilder);
                case AccumulatorN::AccumulatorType::kBottom:
                    return mergeAccStmtFor<TopBottomSense::kBottom, true>(
                        getExpCtx(), accStmts, key.n, key.sortPattern, accIndices, prjArgsBuilder);
                case AccumulatorN::AccumulatorType::kBottomN:
                    return mergeAccStmtFor<TopBottomSense::kBottom, false>(
                        getExpCtx(), accStmts, key.n, key.sortPattern, accIndices, prjArgsBuilder);
                default:
                    MONGO_UNREACHABLE;
            }
        }();
        newAccStmts.push_back(std::move(mergedAccStmt));
    }

    accStmts = std::move(newAccStmts);
    auto prjStageSpec = prjArgsBuilder.done();
    auto prjStage = DocumentSourceProject::create(
        std::move(prjStageSpec), getExpCtx(), DocumentSourceProject::kStageName);
    container->insert(std::next(itr), prjStage);

    return true;
}

bool DocumentSourceGroup::tryToOptimizeAccN(DocumentSourceContainer::iterator itr,
                                            DocumentSourceContainer* container) {
    auto& accumulators = getMutableAccumulationStatements();
    if (accumulators.empty()) {
        return false;
    }

    const auto firstAcc = accumulators[0];
    const auto firstAccInfoItr = kAccNameToInfoMap.find(firstAcc.expr.name);
    if (firstAccInfoItr == kAccNameToInfoMap.end()) {
        return false;
    }

    bool foundEligibleMultiAccs = false;
    boost::optional<SortPattern> sortPattern;
    if (std::holds_alternative<TopBottomSense>(firstAccInfoItr->second.sense)) {
        sortPattern = getAccSortPattern(firstAcc.makeAccumulator());
    }

    for (const auto& acc : accumulators) {
        const auto currentAccInfoItr = kAccNameToInfoMap.find(acc.expr.name);
        if (currentAccInfoItr == kAccNameToInfoMap.end()) {
            return false;
        }

        // All accumulators should be compatible (e.g. all $top's and $topN's).
        if (firstAccInfoItr->second.sense != currentAccInfoItr->second.sense) {
            return false;
        }

        // Sort patterns must match (in case of $top/$topN/$bottom/$bottomN).
        if (sortPattern && *sortPattern != getAccSortPattern(acc.makeAccumulator())) {
            return false;
        }

        // The remaining logic is only intended for multi accs ($firstN/$lastN/$topN/$bottomN).
        if (!currentAccInfoItr->second.multi) {
            continue;
        }

        // The optimization only applies if N == 1.
        const auto init = acc.expr.initializer;
        if (const auto constInit = dynamic_cast<ExpressionConstant*>(init.get()); constInit) {
            const auto constVal = constInit->getValue();
            if (!constVal.numeric() || constVal.coerceToLong() != 1) {
                return false;
            }
        } else {
            return false;
        }

        foundEligibleMultiAccs = true;
    }

    // If there are no eligible multi accs present (e.g. there are only $top's), no conversion needs
    // to happen.
    if (!foundEligibleMultiAccs) {
        return false;
    }

    // Converts $firstN to $first, $lastN to $last, $topN to $top or $bottomN to $bottom.
    // Also adds an $addFields stage to wrap the output of each accumulator in an array. Note that
    // this does not add new fields, but merely overwrites the existing field(s). For example:
    // {$addFields: {myField: ["$myField"]}.
    const auto convertAccFunc =
        createAccConversionFunction(getExpCtx(), firstAccInfoItr->second.sense, sortPattern);
    BSONObjBuilder addFieldsArgsBuilder;
    for (auto& acc : accumulators) {
        // Non-multi accumulators (e.g. $top) don't need conversion.
        if (!kAccNameToInfoMap.at(acc.expr.name).multi) {
            continue;
        }

        acc = convertAccFunc(acc);

        BSONArrayBuilder arrBuilder;
        arrBuilder.append("$" + acc.fieldName);
        addFieldsArgsBuilder.append(acc.fieldName, arrBuilder.arr());
    }

    auto addFieldsStageSpec = addFieldsArgsBuilder.done();
    auto addFieldsStage = DocumentSourceAddFields::create(
        std::move(addFieldsStageSpec), getExpCtx(), DocumentSourceAddFields::kStageName);
    container->insert(std::next(itr), addFieldsStage);

    return true;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBsonWithMaxMemoryUsage(
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceGroup> groupStage(
        new DocumentSourceGroup(expCtx, maxMemoryUsageBytes));
    groupStage->initializeFromBson(elem);
    return groupStage;
}

}  // namespace mongo
