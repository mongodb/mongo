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

#include <absl/container/flat_hash_map.h>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <iterator>
#include <list>
#include <memory>
#include <type_traits>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::string;
using std::unique_ptr;
using std::vector;

REGISTER_DOCUMENT_SOURCE(match,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceMatch::createFromBson,
                         AllowedWithApiStrict::kAlways);

ALLOCATE_DOCUMENT_SOURCE_ID(match, DocumentSourceMatch::id)

bool DocumentSourceMatch::containsTextOperator(const MatchExpression& expr) {
    if (expr.matchType() == MatchExpression::MatchType::TEXT)
        return true;
    for (auto child : expr) {
        if (containsTextOperator(*child))
            return true;
    }
    return false;
}

DocumentSourceMatch::DocumentSourceMatch(std::unique_ptr<MatchExpression> expr,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {
    auto bsonObj = expr->serialize();
    rebuild(std::move(bsonObj), std::move(expr));
}

DocumentSourceMatch::DocumentSourceMatch(const BSONObj& query,
                                         const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {
    rebuild(query);
}

void DocumentSourceMatch::rebuild(BSONObj predicate) {
    predicate = predicate.getOwned();
    SbeCompatibility originalSbeCompatibility =
        getExpCtx()->sbeCompatibilityExchange(SbeCompatibility::noRequirements);
    ON_BLOCK_EXIT([&] { getExpCtx()->setSbeCompatibility(originalSbeCompatibility); });
    std::unique_ptr<MatchExpression> expr = uassertStatusOK(MatchExpressionParser::parse(
        predicate, getExpCtx(), ExtensionsCallbackNoop(), Pipeline::kAllowedMatcherFeatures));
    _sbeCompatibility = getExpCtx()->getSbeCompatibility();
    rebuild(std::move(predicate), std::move(expr));
}

void DocumentSourceMatch::rebuild(BSONObj predicate, std::unique_ptr<MatchExpression> expr) {
    invariant(predicate.isOwned());
    _isTextQuery = containsTextOperator(*expr);
    DepsTracker dependencies =
        DepsTracker(_isTextQuery ? DepsTracker::kOnlyTextScore : DepsTracker::kNoMetadata);
    getDependencies(expr.get(), &dependencies);
    _matchProcessor = std::make_shared<MatchProcessor>(
        std::move(expr), std::move(dependencies), std::move(predicate));
}

const char* DocumentSourceMatch::getSourceName() const {
    return kStageName.data();
}

Value DocumentSourceMatch::serialize(const SerializationOptions& opts) const {
    if (opts.isSerializingForExplain() || opts.isSerializingForQueryStats()) {
        return Value(
            DOC(getSourceName() << Document(_matchProcessor->getExpression()->serialize(opts))));
    }
    return Value(DOC(getSourceName() << Document(getQuery())));
}

intrusive_ptr<DocumentSource> DocumentSourceMatch::optimize() {
    if (getQuery().isEmpty()) {
        return nullptr;
    }

    _matchProcessor->setExpression(
        optimizeMatchExpression(std::move(_matchProcessor->getExpression()),
                                /* enableSimplification */ false));

    return this;
}

DocumentSourceContainer::iterator DocumentSourceMatch::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    // Since a text search must use an index, it must be the first stage in the pipeline. We cannot
    // combine a non-text stage with a text stage, as that may turn an invalid pipeline into a
    // valid one, unbeknownst to the user.
    if (nextMatch) {
        // Text queries are not allowed anywhere except as the first stage. This is checked before
        // optimization.
        invariant(!nextMatch->_isTextQuery);

        // Merge 'nextMatch' into this stage.
        joinMatchWith(nextMatch, MatchExpression::MatchType::AND);

        // Erase 'nextMatch'.
        container->erase(std::next(itr));

        return itr == container->begin() ? itr : std::prev(itr);
    }
    return std::next(itr);
}

namespace {
// This block contains the functions that make up the implementation of
// DocumentSourceMatch::redactSafePortion(). They will only be called after
// the Match expression has been successfully parsed so they can assume that
// input is well formed.

bool isFieldnameRedactSafe(StringData fieldName) {
    // Can't have numeric elements in the dotted path since redacting elements from an array
    // would change the indexes.

    const size_t dotPos = fieldName.find('.');
    if (dotPos == string::npos)
        return fieldName.empty() || !str::isAllDigits(fieldName);

    const StringData part = fieldName.substr(0, dotPos);
    const StringData rest = fieldName.substr(dotPos + 1);
    return (part.empty() || !str::isAllDigits(part)) && isFieldnameRedactSafe(rest);
}

bool isTypeRedactSafeInComparison(BSONType type) {
    if (type == BSONType::array)
        return false;
    if (type == BSONType::object)
        return false;
    if (type == BSONType::null)
        return false;
    if (type == BSONType::undefined)
        return false;  // Currently a parse error.

    return true;
}

Document redactSafePortionTopLevel(BSONObj query);  // mutually recursive with next function

// Returns the redact-safe portion of an "inner" match expression. This is the layer like
// {$gt: 5} which does not include the field name. Returns an empty document if none of the
// expression can safely be promoted in front of a $redact.
Document redactSafePortionDollarOps(BSONObj expr) {
    MutableDocument output;
    for (auto&& field : expr) {
        if (field.fieldName()[0] != '$')
            continue;

        auto keyword = MatchExpressionParser::parsePathAcceptingKeyword(field);
        if (!keyword) {
            continue;
        }

        switch (*keyword) {
            // These are always ok
            case PathAcceptingKeyword::TYPE:
            case PathAcceptingKeyword::REGEX:
            case PathAcceptingKeyword::OPTIONS:
            case PathAcceptingKeyword::MOD:
            case PathAcceptingKeyword::BITS_ALL_SET:
            case PathAcceptingKeyword::BITS_ALL_CLEAR:
            case PathAcceptingKeyword::BITS_ANY_SET:
            case PathAcceptingKeyword::BITS_ANY_CLEAR:
                output[field.fieldNameStringData()] = Value(field);
                break;

            // These are ok if the type of the rhs is allowed in comparisons
            case PathAcceptingKeyword::EQUALITY:
            case PathAcceptingKeyword::LESS_THAN_OR_EQUAL:
            case PathAcceptingKeyword::GREATER_THAN_OR_EQUAL:
            case PathAcceptingKeyword::LESS_THAN:
            case PathAcceptingKeyword::GREATER_THAN:
                if (isTypeRedactSafeInComparison(field.type()))
                    output[field.fieldNameStringData()] = Value(field);
                break;

            // $in must be all-or-nothing (like $or). Can't include subset of elements.
            case PathAcceptingKeyword::IN_EXPR: {
                bool allOk = true;
                for (auto&& elem : field.Obj()) {
                    if (!isTypeRedactSafeInComparison(elem.type())) {
                        allOk = false;
                        break;
                    }
                }
                if (allOk) {
                    output[field.fieldNameStringData()] = Value(field);
                }

                break;
            }

            case PathAcceptingKeyword::ALL: {
                // $all can include subset of elements (like $and).
                vector<Value> matches;
                for (auto&& elem : field.Obj()) {
                    // NOTE this currently doesn't allow {$all: [{$elemMatch: {...}}]}
                    if (isTypeRedactSafeInComparison(elem.type())) {
                        matches.push_back(Value(elem));
                    }
                }
                if (!matches.empty())
                    output[field.fieldNameStringData()] = Value(std::move(matches));

                break;
            }

            case PathAcceptingKeyword::ELEM_MATCH: {
                BSONObj subIn = field.Obj();
                Document subOut;
                if (subIn.firstElementFieldName()[0] == '$') {
                    subOut = redactSafePortionDollarOps(subIn);
                } else {
                    subOut = redactSafePortionTopLevel(subIn);
                }

                if (!subOut.empty())
                    output[field.fieldNameStringData()] = Value(subOut);

                break;
            }

            // These are never allowed
            case PathAcceptingKeyword::EXISTS:
            case PathAcceptingKeyword::GEO_INTERSECTS:
            case PathAcceptingKeyword::GEO_NEAR:
            case PathAcceptingKeyword::INTERNAL_EXPR_EQ:
            case PathAcceptingKeyword::INTERNAL_EXPR_GT:
            case PathAcceptingKeyword::INTERNAL_EXPR_GTE:
            case PathAcceptingKeyword::INTERNAL_EXPR_LT:
            case PathAcceptingKeyword::INTERNAL_EXPR_LTE:
            case PathAcceptingKeyword::INTERNAL_EQ_HASHED_KEY:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_EQ:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_FMOD:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_ITEMS:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_LENGTH:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_OBJECT_MATCH:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_TYPE:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_UNIQUE_ITEMS:
            case PathAcceptingKeyword::NOT_EQUAL:
            case PathAcceptingKeyword::NOT_IN:
            case PathAcceptingKeyword::SIZE:
            case PathAcceptingKeyword::WITHIN:
                continue;
        }
    }
    return output.freeze();
}

// Returns the redact-safe portion of an "outer" match expression. This is the layer like
// {fieldName: {...}} which does include the field name. Returns an empty document if none of
// the expression can safely be promoted in front of a $redact.
Document redactSafePortionTopLevel(BSONObj query) {
    MutableDocument output;
    for (BSONElement field : query) {
        StringData fieldName = field.fieldNameStringData();
        if (fieldName.starts_with("$")) {
            if (fieldName == "$or") {
                // $or must be all-or-nothing (line $in). Can't include subset of elements.
                vector<Value> okClauses;
                for (auto&& elem : field.Obj()) {
                    Document clause = redactSafePortionTopLevel(elem.Obj());
                    if (clause.empty()) {
                        okClauses.clear();
                        break;
                    }
                    okClauses.push_back(Value(clause));
                }

                if (!okClauses.empty())
                    output["$or"] = Value(std::move(okClauses));
            } else if (fieldName == "$and") {
                // $and can include subset of elements (like $all).
                vector<Value> okClauses;
                for (auto&& elem : field.Obj()) {
                    Document clause = redactSafePortionTopLevel(elem.Obj());
                    if (!clause.empty())
                        okClauses.push_back(Value(clause));
                }
                if (!okClauses.empty())
                    output["$and"] = Value(std::move(okClauses));
            }

            continue;
        }

        if (!isFieldnameRedactSafe(field.fieldNameStringData()))
            continue;

        switch (field.type()) {
            case BSONType::array:
                continue;  // exact matches on arrays are never allowed
            case BSONType::null:
                continue;  // can't look for missing fields
            case BSONType::undefined:
                continue;  // Currently a parse error.

            case BSONType::object: {
                Document sub = redactSafePortionDollarOps(field.Obj());
                if (!sub.empty())
                    output[field.fieldNameStringData()] = Value(sub);

                break;
            }

            // All other types are ok to pass through
            default:
                output[field.fieldNameStringData()] = Value(field);
                break;
        }
    }
    return output.freeze();
}
}  // namespace

BSONObj DocumentSourceMatch::redactSafePortion() const {
    return redactSafePortionTopLevel(getQuery()).toBson();
}

bool DocumentSourceMatch::isTextQuery(const BSONObj& query) {
    for (auto&& e : query) {
        const StringData fieldName = e.fieldNameStringData();
        if (fieldName == "$text"_sd)
            return true;

        if (e.isABSONObj() && isTextQuery(e.Obj()))
            return true;
    }
    return false;
}

void DocumentSourceMatch::joinMatchWith(intrusive_ptr<DocumentSourceMatch> other,
                                        MatchExpression::MatchType joinPred) {
    tassert(9912100,
            "joinPred must be MatchExpression::MatchType::AND or MatchExpression::MatchType::OR",
            joinPred == MatchExpression::MatchType::AND ||
                joinPred == MatchExpression::MatchType::OR);

    BSONObjBuilder bob;
    BSONArrayBuilder arrBob(
        bob.subarrayStart(joinPred == MatchExpression::MatchType::AND ? "$and" : "$or"));
    auto addPredicates = [&](const auto& predicates) {
        if (predicates.isEmpty()) {
            arrBob.append(predicates);
        }
        for (auto&& pred : predicates) {
            // For 'joinPred' == $and: If 'pred' is an $and, add its children directly to the
            // new top-level $and to avoid nesting $and's. For 'joinPred' == $or: If 'pred' is a
            // $or, add its children directly to the new top-level $or to avoid nesting $or's.
            // Otherwise, add 'pred' itself as a child.
            if ((joinPred == MatchExpression::MatchType::AND &&
                 pred.fieldNameStringData() == "$and") ||
                (joinPred == MatchExpression::MatchType::OR &&
                 pred.fieldNameStringData() == "$or")) {
                for (auto& child : pred.Array()) {
                    arrBob.append(child);
                }
            } else {
                BSONObjBuilder childBob(arrBob.subobjStart());
                childBob.append(pred);
            }
        }
    };
    addPredicates(_matchProcessor->getPredicate());
    addPredicates(other->_matchProcessor->getPredicate());

    arrBob.doneFast();
    rebuild(bob.obj());
}
pair<intrusive_ptr<DocumentSourceMatch>, intrusive_ptr<DocumentSourceMatch>>
DocumentSourceMatch::splitSourceBy(const OrderedPathSet& fields,
                                   const StringMap<std::string>& renames) && {
    return std::move(*this).splitSourceByFunc(fields, renames, expression::isIndependentOf);
}

pair<intrusive_ptr<DocumentSourceMatch>, intrusive_ptr<DocumentSourceMatch>>
DocumentSourceMatch::splitSourceByFunc(const OrderedPathSet& fields,
                                       const StringMap<std::string>& renames,
                                       expression::ShouldSplitExprFunc func) && {
    pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> newExpr(
        expression::splitMatchExpressionBy(
            std::move(_matchProcessor->getExpression()), fields, renames, func));

    invariant(newExpr.first || newExpr.second);

    if (!newExpr.first) {
        // The entire $match depends on 'fields'. It cannot be split or moved, so we return this
        // stage without modification as the second stage in the pair.
        _matchProcessor->setExpression(std::move(newExpr.second));
        return {nullptr, this};
    }

    if (!newExpr.second && renames.empty()) {
        // This $match is entirely independent of 'fields' and there were no renames to apply. In
        // this case, the current stage can swap with its predecessor without modification. We
        // simply return this as the first stage in the pair.
        _matchProcessor->setExpression(std::move(newExpr.first));
        return {this, nullptr};
    }

    // If we're here, then either:
    //  - this stage has split into two, or
    //  - this stage can swap with its predecessor, but potentially had renames applied.
    //
    // In any of these cases, we have created new expression(s). A MatchExpression requires that it
    // is outlived by the BSONObj it is parsed from. But since the MatchExpressions were modified,
    // the corresponding BSONObj may not exist. Therefore, we take each of these expressions,
    // serialize them, and then re-parse them, constructing new BSON that is owned by the
    // DocumentSourceMatch.
    auto firstMatch = DocumentSourceMatch::create(newExpr.first->serialize(), getExpCtx());

    intrusive_ptr<DocumentSourceMatch> secondMatch;
    if (newExpr.second) {
        secondMatch = DocumentSourceMatch::create(newExpr.second->serialize(), getExpCtx());
    }

    return {std::move(firstMatch), std::move(secondMatch)};
}

boost::intrusive_ptr<DocumentSourceMatch> DocumentSourceMatch::descendMatchOnPath(
    const MatchExpression* matchExpr,
    const std::string& descendOn,
    const intrusive_ptr<ExpressionContext>& expCtx) {
    std::unique_ptr<MatchExpression> meCopy = matchExpr->clone();
    expression::mapOver(
        meCopy.get(), [&descendOn](MatchExpression* node, std::string path) -> void {
            // Cannot call this method on a $match including a $elemMatch.
            tassert(9224700,
                    "The given match expression has a node that represents a partial path.",
                    !MatchExpression::isInternalNodeWithPath(node->matchType()));
            // Only leaf and array match expressions have a path.
            if (node->getCategory() != MatchExpression::MatchCategory::kLeaf &&
                node->getCategory() != MatchExpression::MatchCategory::kArrayMatching) {
                return;
            }

            auto leafPath = node->path();
            tassert(9224701,
                    str::stream() << "Expected '" << redact(descendOn) << "' to be a prefix of '"
                                  << redact(leafPath) << "', but it is not.",
                    expression::isPathPrefixOf(descendOn, leafPath));

            auto newPath = leafPath.substr(descendOn.size() + 1);
            if (node->getCategory() == MatchExpression::MatchCategory::kLeaf) {
                auto leafNode = static_cast<LeafMatchExpression*>(node);
                leafNode->setPath(newPath);
            } else if (node->getCategory() == MatchExpression::MatchCategory::kArrayMatching) {
                auto arrayNode = static_cast<ArrayMatchingMatchExpression*>(node);
                arrayNode->setPath(newPath);
            }
        });

    return new DocumentSourceMatch(meCopy->serialize(), expCtx);
}

std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
DocumentSourceMatch::splitMatchByModifiedFields(
    const boost::intrusive_ptr<DocumentSourceMatch>& match,
    const DocumentSource::GetModPathsReturn& modifiedPathsRet) {
    // Attempt to move some or all of this $match before this stage.
    OrderedPathSet modifiedPaths;
    switch (modifiedPathsRet.type) {
        case DocumentSource::GetModPathsReturn::Type::kNotSupported:
            // We don't know what paths this stage might modify, so refrain from swapping.
            return {nullptr, match};
        case DocumentSource::GetModPathsReturn::Type::kAllPaths:
            // This stage modifies all paths, so cannot be swapped with a $match at all.
            return {nullptr, match};
        case DocumentSource::GetModPathsReturn::Type::kFiniteSet:
            modifiedPaths = modifiedPathsRet.paths;
            break;
        case DocumentSource::GetModPathsReturn::Type::kAllExcept: {
            DepsTracker depsTracker;
            match->getDependencies(&depsTracker);

            auto preservedPaths = modifiedPathsRet.paths;
            for (auto&& rename : modifiedPathsRet.renames) {
                preservedPaths.insert(rename.first);
            }
            modifiedPaths =
                semantic_analysis::extractModifiedDependencies(depsTracker.fields, preservedPaths)
                    .modified;
        }
    }
    return std::move(*match).splitSourceBy(modifiedPaths, modifiedPathsRet.renames);
}

intrusive_ptr<DocumentSourceMatch> DocumentSourceMatch::create(
    BSONObj filter, const intrusive_ptr<ExpressionContext>& expCtx) {
    intrusive_ptr<DocumentSourceMatch> match(new DocumentSourceMatch(filter, expCtx));
    return match;
}

intrusive_ptr<DocumentSource> DocumentSourceMatch::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15959,
            "the match filter must be an expression in an object",
            elem.type() == BSONType::object);

    return DocumentSourceMatch::create(elem.Obj(), pExpCtx);
}

bool DocumentSourceMatch::hasQuery() const {
    return true;
}

BSONObj DocumentSourceMatch::getQuery() const {
    // Note that we must return the backing BSON of the MatchExpression. This is because we use
    // the result of 'getQuery' during pushdown to the find layer in order to keep alive the
    // MatchExpression's backing BSON, since we use the MatchExpression to construct the
    // CanonicalQuery and require that it holds pointers into valid BSON during execution.
    return _matchProcessor->getPredicate();
}

DepsTracker::State DocumentSourceMatch::getDependencies(DepsTracker* deps) const {
    return getDependencies(_matchProcessor->getExpression().get(), deps);
}

DepsTracker::State DocumentSourceMatch::getDependencies(const MatchExpression* expr,
                                                        DepsTracker* deps) const {
    // Get all field or variable dependencies.
    dependency_analysis::addDependencies(expr, deps);

    if (isTextQuery()) {
        // A $text aggregation field should return EXHAUSTIVE_FIELDS, since we don't necessarily
        // know what field it will be searching without examining indices.
        deps->needWholeDocument = true;

        // This may look confusing, but we must call two setters on the DepsTracker for different
        // purposes. We mark "textScore" as available metadata that can be consumed by any
        // downstream stage for $meta field validation. We also also mark that this stage does
        // require "textScore" so that the executor knows to produce the metadata.
        // TODO SERVER-100902 Split $meta validation out of dependency tracking.
        deps->setMetadataAvailable(DocumentMetadataFields::kTextScore);
        deps->setNeedsMetadata(DocumentMetadataFields::kTextScore);
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    return DepsTracker::State::SEE_NEXT;
}

void DocumentSourceMatch::addVariableRefs(std::set<Variables::Id>* refs) const {
    dependency_analysis::addVariableRefs(_matchProcessor->getExpression().get(), refs);
}

Value DocumentSourceInternalChangeStreamMatch::serialize(const SerializationOptions& opts) const {
    if (opts.isSerializingForQueryStats()) {
        // Stages made internally by 'DocumentSourceChangeStream' should not be serialized for
        // query stats. For query stats we will serialize only the user specified $changeStream
        // stage.
        return Value();
    }
    return doSerialize(opts);
}

StageConstraints DocumentSourceInternalChangeStreamMatch::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kAllowed,
                                 TransactionRequirement::kAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed,
                                 ChangeStreamRequirement::kAllowlist};
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

intrusive_ptr<DocumentSourceInternalChangeStreamMatch>
DocumentSourceInternalChangeStreamMatch::create(BSONObj filter,
                                                const intrusive_ptr<ExpressionContext>& expCtx) {
    intrusive_ptr<DocumentSourceInternalChangeStreamMatch> internalMatch(
        new DocumentSourceInternalChangeStreamMatch(filter, expCtx));
    return internalMatch;
}

}  // namespace mongo
