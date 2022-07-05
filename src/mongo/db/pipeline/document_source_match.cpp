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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_match.h"

#include <algorithm>
#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/util/ctype.h"
#include "mongo/util/str.h"

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

const char* DocumentSourceMatch::getSourceName() const {
    return kStageName.rawData();
}

Value DocumentSourceMatch::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        BSONObjBuilder builder;
        _expression->serialize(&builder);
        return Value(DOC(getSourceName() << Document(builder.obj())));
    }
    return Value(DOC(getSourceName() << Document(getQuery())));
}

intrusive_ptr<DocumentSource> DocumentSourceMatch::optimize() {
    if (getQuery().isEmpty()) {
        return nullptr;
    }

    _expression = MatchExpression::optimize(std::move(_expression));

    return this;
}

DocumentSource::GetNextResult DocumentSourceMatch::doGetNext() {
    // The user facing error should have been generated earlier.
    massert(17309, "Should never call getNext on a $match stage with $text clause", !_isTextQuery);

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        // MatchExpression only takes BSON documents, so we have to make one. As an optimization,
        // only serialize the fields we need to do the match.
        BSONObj toMatch = _dependencies.needWholeDocument
            ? nextInput.getDocument().toBson()
            : document_path_support::documentToBsonWithPaths(nextInput.getDocument(),
                                                             _dependencies.fields);

        if (_expression->matchesBSON(toMatch)) {
            return nextInput;
        }

        // For performance reasons, a streaming stage must not keep references to documents across
        // calls to getNext(). Such stages must retrieve a result from their child and then release
        // it (or return it) before asking for another result. Failing to do so can result in extra
        // work, since the Document/Value library must copy data on write when that data has a
        // refcount above one.
        nextInput.releaseDocument();
    }

    return nextInput;
}

Pipeline::SourceContainer::iterator DocumentSourceMatch::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
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
        joinMatchWith(nextMatch);

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

bool isAllDigits(StringData str) {
    return !str.empty() &&
        std::all_of(str.begin(), str.end(), [](char c) { return ctype::isDigit(c); });
}

bool isFieldnameRedactSafe(StringData fieldName) {
    // Can't have numeric elements in the dotted path since redacting elements from an array
    // would change the indexes.

    const size_t dotPos = fieldName.find('.');
    if (dotPos == string::npos)
        return !isAllDigits(fieldName);

    const StringData part = fieldName.substr(0, dotPos);
    const StringData rest = fieldName.substr(dotPos + 1);
    return !isAllDigits(part) && isFieldnameRedactSafe(rest);
}

bool isTypeRedactSafeInComparison(BSONType type) {
    if (type == Array)
        return false;
    if (type == Object)
        return false;
    if (type == jstNULL)
        return false;
    if (type == Undefined)
        return false;  // Currently a parse error.

    return true;
}

Document redactSafePortionTopLevel(BSONObj query);  // mutually recursive with next function

// Returns the redact-safe portion of an "inner" match expression. This is the layer like
// {$gt: 5} which does not include the field name. Returns an empty document if none of the
// expression can safely be promoted in front of a $redact.
Document redactSafePortionDollarOps(BSONObj expr) {
    MutableDocument output;
    BSONForEach(field, expr) {
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
                BSONForEach(elem, field.Obj()) {
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
                BSONForEach(elem, field.Obj()) {
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
        if (fieldName.startsWith("$")) {
            if (fieldName == "$or") {
                // $or must be all-or-nothing (line $in). Can't include subset of elements.
                vector<Value> okClauses;
                BSONForEach(elem, field.Obj()) {
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
                BSONForEach(elem, field.Obj()) {
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
            case Array:
                continue;  // exact matches on arrays are never allowed
            case jstNULL:
                continue;  // can't look for missing fields
            case Undefined:
                continue;  // Currently a parse error.

            case Object: {
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
    BSONForEach(e, query) {
        const StringData fieldName = e.fieldNameStringData();
        if (fieldName == "$text"_sd)
            return true;

        if (e.isABSONObj() && isTextQuery(e.Obj()))
            return true;
    }
    return false;
}

void DocumentSourceMatch::joinMatchWith(intrusive_ptr<DocumentSourceMatch> other) {
    BSONObjBuilder bob;
    BSONArrayBuilder arrBob(bob.subarrayStart("$and"));

    auto addPredicates = [&](const auto& predicates) {
        if (predicates.isEmpty()) {
            arrBob.append(predicates);
        }

        for (auto&& pred : predicates) {
            // If 'pred' is an $and, add its children directly to the new top-level $and to avoid
            // nesting $and's. Otherwise, add 'pred' itself as a child.
            if (pred.fieldNameStringData() == "$and") {
                for (auto& child : pred.Array()) {
                    arrBob.append(child);
                }
            } else {
                BSONObjBuilder childBob(arrBob.subobjStart());
                childBob.append(pred);
            }
        }
    };

    addPredicates(_predicate);
    addPredicates(other->_predicate);

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
        expression::splitMatchExpressionBy(std::move(_expression), fields, renames, func));

    invariant(newExpr.first || newExpr.second);

    if (!newExpr.first) {
        // The entire $match depends on 'fields'. It cannot be split or moved, so we return this
        // stage without modification as the second stage in the pair.
        _expression = std::move(newExpr.second);
        return {nullptr, this};
    }

    if (!newExpr.second && renames.empty()) {
        // This $match is entirely independent of 'fields' and there were no renames to apply. In
        // this case, the current stage can swap with its predecessor without modification. We
        // simply return this as the first stage in the pair.
        _expression = std::move(newExpr.first);
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
    BSONObjBuilder firstBob;
    newExpr.first->serialize(&firstBob);
    auto firstMatch = DocumentSourceMatch::create(firstBob.obj(), pExpCtx);

    intrusive_ptr<DocumentSourceMatch> secondMatch;
    if (newExpr.second) {
        BSONObjBuilder secondBob;
        newExpr.second->serialize(&secondBob);
        secondMatch = DocumentSourceMatch::create(secondBob.obj(), pExpCtx);
    }

    return {std::move(firstMatch), std::move(secondMatch)};
}

boost::intrusive_ptr<DocumentSourceMatch> DocumentSourceMatch::descendMatchOnPath(
    MatchExpression* matchExpr,
    const std::string& descendOn,
    const intrusive_ptr<ExpressionContext>& expCtx) {
    expression::mapOver(matchExpr, [&descendOn](MatchExpression* node, std::string path) -> void {
        // Cannot call this method on a $match including a $elemMatch.
        invariant(node->matchType() != MatchExpression::ELEM_MATCH_OBJECT &&
                  node->matchType() != MatchExpression::ELEM_MATCH_VALUE);
        // Only leaf and array match expressions have a path.
        if (node->getCategory() != MatchExpression::MatchCategory::kLeaf &&
            node->getCategory() != MatchExpression::MatchCategory::kArrayMatching) {
            return;
        }

        auto leafPath = node->path();
        invariant(expression::isPathPrefixOf(descendOn, leafPath));

        auto newPath = leafPath.substr(descendOn.size() + 1);
        if (node->getCategory() == MatchExpression::MatchCategory::kLeaf) {
            auto leafNode = static_cast<LeafMatchExpression*>(node);
            leafNode->setPath(newPath);
        } else if (node->getCategory() == MatchExpression::MatchCategory::kArrayMatching) {
            auto arrayNode = static_cast<ArrayMatchingMatchExpression*>(node);
            arrayNode->setPath(newPath);
        }
    });

    BSONObjBuilder query;
    matchExpr->serialize(&query);
    return new DocumentSourceMatch(query.obj(), expCtx);
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
            modifiedPaths = std::move(modifiedPathsRet.paths);
            break;
        case DocumentSource::GetModPathsReturn::Type::kAllExcept: {
            DepsTracker depsTracker;
            match->getDependencies(&depsTracker);

            auto preservedPaths = modifiedPathsRet.paths;
            for (auto&& rename : modifiedPathsRet.renames) {
                preservedPaths.insert(rename.first);
            }
            modifiedPaths =
                semantic_analysis::extractModifiedDependencies(depsTracker.fields, preservedPaths);
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
    uassert(15959, "the match filter must be an expression in an object", elem.type() == Object);

    return DocumentSourceMatch::create(elem.Obj(), pExpCtx);
}

bool DocumentSourceMatch::hasQuery() const {
    return true;
}

BSONObj DocumentSourceMatch::getQuery() const {
    return _predicate;
}

DepsTracker::State DocumentSourceMatch::getDependencies(DepsTracker* deps) const {
    // Get all field or variable dependencies.
    _expression->addDependencies(deps);

    if (isTextQuery()) {
        // A $text aggregation field should return EXHAUSTIVE_FIELDS, since we don't necessarily
        // know what field it will be searching without examining indices.
        deps->needWholeDocument = true;
        deps->setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    return DepsTracker::State::SEE_NEXT;
}

DocumentSourceMatch::DocumentSourceMatch(const BSONObj& query,
                                         const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {
    rebuild(query);
}

void DocumentSourceMatch::rebuild(BSONObj filter) {
    _predicate = filter.getOwned();
    _expression = uassertStatusOK(MatchExpressionParser::parse(
        _predicate, pExpCtx, ExtensionsCallbackNoop(), Pipeline::kAllowedMatcherFeatures));
    _isTextQuery = isTextQuery(_predicate);
    _dependencies =
        DepsTracker(_isTextQuery ? DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore
                                 : DepsTracker::kAllMetadata);
    getDependencies(&_dependencies);
}

}  // namespace mongo
