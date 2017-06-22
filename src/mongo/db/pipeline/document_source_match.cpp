/**
*    Copyright (C) 2011 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_match.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::unique_ptr;
using std::string;
using std::vector;

REGISTER_DOCUMENT_SOURCE(match,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceMatch::createFromBson);

const char* DocumentSourceMatch::getSourceName() const {
    return "$match";
}

Value DocumentSourceMatch::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(DOC(getSourceName() << Document(getQuery())));
}

intrusive_ptr<DocumentSource> DocumentSourceMatch::optimize() {
    return getQuery().isEmpty() ? nullptr : this;
}

DocumentSource::GetNextResult DocumentSourceMatch::getNext() {
    pExpCtx->checkForInterrupt();

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
    if (str.empty())
        return false;

    for (size_t i = 0; i < str.size(); i++) {
        if (!isdigit(str[i]))
            return false;
    }
    return true;
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

        if (field.fieldNameStringData() == "$eq") {
            if (isTypeRedactSafeInComparison(field.type())) {
                output[field.fieldNameStringData()] = Value(field);
            }
            continue;
        }

        switch (*MatchExpressionParser::parsePathAcceptingKeyword(field,
                                                                  PathAcceptingKeyword::EQUALITY)) {
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
            case PathAcceptingKeyword::EQUALITY:  // This actually means unknown
            case PathAcceptingKeyword::GEO_NEAR:
            case PathAcceptingKeyword::NOT_EQUAL:
            case PathAcceptingKeyword::SIZE:
            case PathAcceptingKeyword::NOT_IN:
            case PathAcceptingKeyword::EXISTS:
            case PathAcceptingKeyword::WITHIN:
            case PathAcceptingKeyword::GEO_INTERSECTS:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_ITEMS:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_UNIQUE_ITEMS:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_OBJECT_MATCH:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_LENGTH:
            case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH:
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
    BSONForEach(field, query) {
        if (field.fieldName()[0] == '$') {
            if (str::equals(field.fieldName(), "$or")) {
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
            } else if (str::equals(field.fieldName(), "$and")) {
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
}

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
    _predicate = BSON("$and" << BSON_ARRAY(_predicate << other->getQuery()));

    StatusWithMatchExpression status = uassertStatusOK(
        MatchExpressionParser::parse(_predicate, ExtensionsCallbackNoop(), pExpCtx->getCollator()));
    _expression = std::move(status.getValue());
    _dependencies = DepsTracker(_dependencies.getMetadataAvailable());
    getDependencies(&_dependencies);
}

pair<intrusive_ptr<DocumentSourceMatch>, intrusive_ptr<DocumentSourceMatch>>
DocumentSourceMatch::splitSourceBy(const std::set<std::string>& fields,
                                   const StringMap<std::string>& renames) {
    pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> newExpr(
        expression::splitMatchExpressionBy(std::move(_expression), fields, renames));

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
        if (node->getCategory() == MatchExpression::MatchCategory::kLeaf &&
            node->matchType() != MatchExpression::TYPE_OPERATOR) {
            auto leafNode = static_cast<LeafMatchExpression*>(node);
            leafNode->setPath(newPath).transitional_ignore();
        } else if (node->getCategory() == MatchExpression::MatchCategory::kArrayMatching) {
            auto arrayNode = static_cast<ArrayMatchingMatchExpression*>(node);
            arrayNode->setPath(newPath).transitional_ignore();
        }
    });

    BSONObjBuilder query;
    matchExpr->serialize(&query);
    return new DocumentSourceMatch(query.obj(), expCtx);
}

static void uassertNoDisallowedClauses(BSONObj query) {
    BSONForEach(e, query) {
        // can't use the MatchExpression API because this would segfault the constructor
        uassert(16395,
                "$where is not allowed inside of a $match aggregation expression",
                !str::equals(e.fieldName(), "$where"));
        // geo breaks if it is not the first portion of the pipeline
        uassert(16424,
                "$near is not allowed inside of a $match aggregation expression",
                !str::equals(e.fieldName(), "$near"));
        uassert(16426,
                "$nearSphere is not allowed inside of a $match aggregation expression",
                !str::equals(e.fieldName(), "$nearSphere"));
        if (e.isABSONObj())
            uassertNoDisallowedClauses(e.Obj());
    }
}

intrusive_ptr<DocumentSourceMatch> DocumentSourceMatch::create(
    BSONObj filter, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassertNoDisallowedClauses(filter);
    intrusive_ptr<DocumentSourceMatch> match(new DocumentSourceMatch(filter, expCtx));
    return match;
}

intrusive_ptr<DocumentSource> DocumentSourceMatch::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15959, "the match filter must be an expression in an object", elem.type() == Object);

    return DocumentSourceMatch::create(elem.Obj(), pExpCtx);
}

BSONObj DocumentSourceMatch::getQuery() const {
    return _predicate;
}

DocumentSource::GetDepsReturn DocumentSourceMatch::getDependencies(DepsTracker* deps) const {
    if (isTextQuery()) {
        // A $text aggregation field should return EXHAUSTIVE_ALL, since we don't necessarily know
        // what field it will be searching without examining indices.
        deps->needWholeDocument = true;
        return EXHAUSTIVE_ALL;
    }

    addDependencies(deps);
    return SEE_NEXT;
}

void DocumentSourceMatch::addDependencies(DepsTracker* deps) const {
    expression::mapOver(_expression.get(), [deps](MatchExpression* node, std::string path) -> void {
        if (!path.empty() &&
            (node->numChildren() == 0 || node->matchType() == MatchExpression::ELEM_MATCH_VALUE ||
             node->matchType() == MatchExpression::ELEM_MATCH_OBJECT)) {
            deps->fields.insert(path);
        }
    });
}

DocumentSourceMatch::DocumentSourceMatch(const BSONObj& query,
                                         const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), _predicate(query.getOwned()), _isTextQuery(isTextQuery(query)) {
    StatusWithMatchExpression status = uassertStatusOK(
        MatchExpressionParser::parse(_predicate, ExtensionsCallbackNoop(), pExpCtx->getCollator()));

    _expression = std::move(status.getValue());
    getDependencies(&_dependencies);
}

}  // namespace mongo
