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

#include "mongo/db/pipeline/document_source_replace_root.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_replace_root_gen.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <iterator>
#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {

using boost::intrusive_ptr;

Document ReplaceRootTransformation::applyTransformation(const Document& input) const {
    // Extract subdocument in the form of a Value.
    Value newRoot = _newRoot->evaluate(input, &_expCtx->variables);
    // The newRoot expression, if it exists, must evaluate to an object.
    uassert(40228,
            fmt::format(kErrorTemplate.data(),
                        _errMsgContextForNonObject,
                        newRoot.toString(),
                        typeName(newRoot.getType()),
                        input.toString()),
            newRoot.getType() == BSONType::object);

    // Turn the value into a document.
    MutableDocument newDoc(newRoot.getDocument());
    newDoc.copyMetaDataFrom(input);
    return newDoc.freeze();
}

boost::optional<std::string> ReplaceRootTransformation::unnestsPath() const {
    // Ensure that expressionFieldPath exists and has a field path longer than 1 before we unnest.
    // We don't want to unnest for {$replacewith: "$$ROOT"}, which has a field length of 1.
    if (const ExpressionFieldPath* const expressionFieldPath =
            dynamic_cast<ExpressionFieldPath*>(getExpression().get());
        expressionFieldPath && expressionFieldPath->getFieldPath().getPathLength() > 1) {
        return expressionFieldPath->getFieldPathWithoutCurrentPrefix().fullPath();
    } else {
        return boost::none;
    }
}

boost::intrusive_ptr<DocumentSourceMatch> ReplaceRootTransformation::createTypeNEObjectPredicate(
    const std::string& expression, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto matchExpr = std::make_unique<OrMatchExpression>();
    {
        MatcherTypeSet typeSet;
        typeSet.bsonTypes.insert(BSONType::array);
        auto typeIsArrayExpr =
            std::make_unique<TypeMatchExpression>(StringData(expression), typeSet);
        matchExpr->add(std::move(typeIsArrayExpr));
    }
    {
        MatcherTypeSet typeSet;
        typeSet.bsonTypes.insert(BSONType::object);
        auto typeIsObjectExpr =
            std::make_unique<TypeMatchExpression>(StringData(expression), typeSet);
        auto typeIsNotObjectExpr =
            std::make_unique<NotMatchExpression>(std::move(typeIsObjectExpr));
        matchExpr->add(std::move(typeIsNotObjectExpr));
    }

    BSONObjBuilder bob;
    matchExpr->serialize(&bob);
    return DocumentSourceMatch::create(bob.obj(), expCtx);
}

void ReplaceRootTransformation::reportRenames(const MatchExpression* expr,
                                              const std::string& prefixPath,
                                              StringMap<std::string>& renames) {
    DepsTracker deps;
    dependency_analysis::addDependencies(expr, &deps);
    for (const auto& path : deps.fields) {
        // Only record renames for top level paths.
        const auto oldPathTopLevelField = FieldPath::extractFirstFieldFromDottedPath(path);
        renames.emplace(std::make_pair(oldPathTopLevelField,
                                       fmt::format("{}.{}", prefixPath, oldPathTopLevelField)));
    }
}

bool ReplaceRootTransformation::pushDotRenamedMatchBefore(DocumentSourceContainer::iterator itr,
                                                          DocumentSourceContainer* container) {
    // Attempt to push match stage before replaceRoot/replaceWith stage.
    const auto prospectiveMatch = dynamic_cast<DocumentSourceMatch*>(std::next(itr)->get());
    const auto unnestedPath = unnestsPath();

    if (prospectiveMatch && unnestedPath) {
        MatchExpression* expr = prospectiveMatch->getMatchExpression();
        // A MatchExpression that contains $expr is ineligible for pushdown because the match
        // pushdown turned out to be comparatively slower in our benchmarks when $expr was used on
        // dotted paths. Since MatchExpressions operate on BSONObj, while agg expressions operate on
        // Documents. $expr also goes through an additional step of converting BSONObj to Document.
        if (QueryPlannerCommon::hasNode(expr, MatchExpression::EXPRESSION)) {
            return false;
        }

        // If we reach this point, we know:
        // 1) The current stage is a ReplaceRoot stage whose transformation represents the unnesting
        // of a field path (length > 1).
        // 2) The stage after us is a non-$expr match. For a replaceRoot stage that unnests a field
        // path, we will attempt to prepend the field path to subpaths in a copy of the match
        // stage's MatchExpression.
        //
        // Ex: For the pipeline [{$replaceWith: {"$subDocument"}}, {$match: {x: 2}}], we make a copy
        // of the original match expression and transform it to {$match: {"subDocument.x": 2}}. If
        // the entire ME is eligible, we return a new match stage with the prepended ME.
        auto& prefixPath = unnestedPath.get();
        StringMap<std::string> renames;

        // We will apply renames to the match stage and perform the swap if the match
        // expression is eligible.
        reportRenames(expr, prefixPath, renames);
        // Report renames in ReplaceRoot stage,
        auto modPaths = getModifiedPaths();
        modPaths = {
            DocumentSource::GetModPathsReturn::Type::kFiniteSet, {prefixPath}, std::move(renames)};

        // Translate predicate statements based on the replace root stage renames.
        auto splitMatchForReplaceRoot =
            DocumentSourceMatch::splitMatchByModifiedFields(prospectiveMatch, modPaths);

        if (splitMatchForReplaceRoot.first) {
            // Join with "type != object" condition. This is to ensure that cases which would have
            // resulted in an error before the optimization are not 'optimized' to cases which do
            // not error. Optimizations should not change the behavior.
            splitMatchForReplaceRoot.first->joinMatchWith(
                createTypeNEObjectPredicate(prefixPath, _expCtx), MatchExpression::MatchType::OR);

            // Swap the eligible portion of the match stage with the replaceRoot stage. std::swap is
            // used here as it performs reassignment of what the iterators point to in O(1) for
            // non-array inputs.
            *std::next(itr) = std::move(splitMatchForReplaceRoot.first);
            std::swap(*itr, *std::next(itr));

            if (splitMatchForReplaceRoot.second) {
                // itr now points to the pushed down, eligible portion of the original match stage.
                // Insert the remaining portion after the replaceRoot stage.
                container->insert(std::next(std::next(itr)),
                                  std::move(splitMatchForReplaceRoot.second));
            }

            return true;
        }
    }

    return false;
}

DocumentSourceContainer::iterator ReplaceRootTransformation::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // Attempt to push match stage before replaceRoot/replaceWith stage.
    if (pushDotRenamedMatchBefore(itr, container)) {
        // Optimize the previous stage. If this is the first stage, optimize the current stage
        // instead.
        return itr == container->begin() ? itr : std::prev(itr);
    }

    return std::next(itr);
}

REGISTER_DOCUMENT_SOURCE(replaceRoot,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceReplaceRoot::createFromBson,
                         AllowedWithApiStrict::kAlways);
REGISTER_DOCUMENT_SOURCE(replaceWith,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceReplaceRoot::createFromBson,
                         AllowedWithApiStrict::kAlways);

intrusive_ptr<DocumentSource> DocumentSourceReplaceRoot::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    const auto stageName = elem.fieldNameStringData();
    SbeCompatibility originalSbeCompatibility =
        expCtx->sbeCompatibilityExchange(SbeCompatibility::noRequirements);
    ON_BLOCK_EXIT([&] { expCtx->setSbeCompatibility(originalSbeCompatibility); });
    auto newRootExpression = [&]() {
        if (stageName == kAliasNameReplaceWith) {
            return Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState);
        }

        invariant(
            stageName == kStageName,
            str::stream() << "Unexpected stage registered with DocumentSourceReplaceRoot parser: "
                          << stageName);
        uassert(40229,
                str::stream() << "expected an object as specification for " << kStageName
                              << " stage, got " << typeName(elem.type()),
                elem.type() == BSONType::object);

        auto spec = ReplaceRootSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

        // The IDL doesn't give us back the type we need to feed into the expression parser, and
        // the expression parser needs the extra state in 'vps' and 'expCtx', so for now we have
        // to adapt the two.
        BSONObj parsingBson = BSON("newRoot" << spec.getNewRoot());
        return Expression::parseOperand(
            expCtx.get(), parsingBson.firstElement(), expCtx->variablesParseState);
    }();

    // Whether this was specified as $replaceWith or $replaceRoot, always use the name $replaceRoot
    // to simplify the serialization process.
    const bool isIndependentOfAnyCollection = false;
    return new DocumentSourceSingleDocumentTransformation(
        expCtx,
        std::make_unique<ReplaceRootTransformation>(
            expCtx,
            newRootExpression,
            (stageName == kStageName) ? "'newRoot' expression " : "'replacement document' ",
            expCtx->getSbeCompatibility()),
        kStageName.data(),
        isIndependentOfAnyCollection);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceReplaceRoot::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Expression>& newRootExpression,
    std::string errMsgContextForNonObjects,
    SbeCompatibility sbeCompatibility) {
    const bool isIndependentOfAnyCollection = false;
    return new DocumentSourceSingleDocumentTransformation(
        expCtx,
        std::make_unique<ReplaceRootTransformation>(expCtx,
                                                    newRootExpression,
                                                    std::move(errMsgContextForNonObjects),
                                                    expCtx->getSbeCompatibility()),
        kStageName.data(),
        isIndependentOfAnyCollection);
}
}  // namespace mongo
