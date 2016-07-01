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

#include <cctype>

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::unique_ptr;
using std::string;
using std::vector;

REGISTER_DOCUMENT_SOURCE(match, DocumentSourceMatch::createFromBson);

const char* DocumentSourceMatch::getSourceName() const {
    return "$match";
}

Value DocumentSourceMatch::serialize(bool explain) const {
    return Value(DOC(getSourceName() << Document(getQuery())));
}

intrusive_ptr<DocumentSource> DocumentSourceMatch::optimize() {
    return getQuery().isEmpty() ? nullptr : this;
}

boost::optional<Document> DocumentSourceMatch::getNext() {
    pExpCtx->checkForInterrupt();

    // The user facing error should have been generated earlier.
    massert(17309, "Should never call getNext on a $match stage with $text clause", !_isTextQuery);

    while (boost::optional<Document> next = pSource->getNext()) {
        // MatchExpression only takes BSON documents, so we have to make one. As an optimization,
        // only serialize the fields we need to do the match.
        if (_dependencies.needWholeDocument) {
            if (_expression->matchesBSON(next->toBson())) {
                return next;
            }
        } else if (_expression->matchesBSON(getObjectForMatch(*next, _dependencies.fields))) {
            return next;
        }
    }

    // We have exhausted the previous source.
    return boost::none;
}

Pipeline::SourceContainer::iterator DocumentSourceMatch::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    // Since a text search must use an index, it must be the first stage in the pipeline. We cannot
    // combine a non-text stage with a text stage, as that may turn an invalid pipeline into a
    // valid one, unbeknownst to the user.
    if (nextMatch && !nextMatch->_isTextQuery) {
        // Merge 'nextMatch' into this stage.
        joinMatchWith(nextMatch);

        // Erase 'nextMatch'.
        container->erase(std::next(itr));

        return itr == container->begin() ? itr : std::prev(itr);
    }
    return std::next(itr);
}

BSONObj DocumentSourceMatch::getObjectForMatch(const Document& input,
                                               const std::set<std::string>& fields) {
    BSONObjBuilder matchObject;

    for (auto&& field : fields) {
        // getNestedField does not handle dotted paths correctly, so instead of retrieving the
        // entire path, we just extract the first element of the path.
        FieldPath path(field);
        auto prefix = path.getFieldName(0);
        if (!matchObject.hasField(prefix)) {
            // Avoid adding the same prefix twice.
            input.getField(prefix).addToBsonObj(&matchObject, prefix);
        }
    }

    return matchObject.obj();
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

        switch (BSONObj::MatchType(field.getGtLtOp(BSONObj::Equality))) {
            // These are always ok
            case BSONObj::opTYPE:
            case BSONObj::opREGEX:
            case BSONObj::opOPTIONS:
            case BSONObj::opMOD:
            case BSONObj::opBITS_ALL_SET:
            case BSONObj::opBITS_ALL_CLEAR:
            case BSONObj::opBITS_ANY_SET:
            case BSONObj::opBITS_ANY_CLEAR:
                output[field.fieldNameStringData()] = Value(field);
                break;

            // These are ok if the type of the rhs is allowed in comparisons
            case BSONObj::LTE:
            case BSONObj::GTE:
            case BSONObj::LT:
            case BSONObj::GT:
                if (isTypeRedactSafeInComparison(field.type()))
                    output[field.fieldNameStringData()] = Value(field);
                break;

            // $in must be all-or-nothing (like $or). Can't include subset of elements.
            case BSONObj::opIN: {
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

            case BSONObj::opALL: {
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

            case BSONObj::opELEM_MATCH: {
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
            case BSONObj::Equality:  // This actually means unknown
            case BSONObj::opNEAR:
            case BSONObj::NE:
            case BSONObj::opSIZE:
            case BSONObj::NIN:
            case BSONObj::opEXISTS:
            case BSONObj::opWITHIN:
            case BSONObj::opGEO_INTERSECTS:
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

void DocumentSourceMatch::setSource(DocumentSource* source) {
    uassert(17313, "$match with $text is only allowed as the first pipeline stage", !_isTextQuery);
    DocumentSource::setSource(source);
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
}

pair<intrusive_ptr<DocumentSource>, intrusive_ptr<DocumentSource>>
DocumentSourceMatch::splitSourceBy(const std::set<std::string>& fields) {
    pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> newExpr(
        expression::splitMatchExpressionBy(std::move(_expression), fields));

    invariant(newExpr.first || newExpr.second);

    if (!newExpr.first) {
        // The entire $match dependends on 'fields'.
        _expression = std::move(newExpr.second);
        return {nullptr, this};
    } else if (!newExpr.second) {
        // This $match is entirely independent of 'fields'.
        _expression = std::move(newExpr.first);
        return {this, nullptr};
    }

    // A MatchExpression requires that it is outlived by the BSONObj it is parsed from. Since the
    // original BSONObj this $match was created from is no longer equivalent to either of the
    // MatchExpressions we return, we instead take each of these expressions, serialize them, and
    // then re-parse them, constructing new BSON that is owned by the DocumentSourceMatch.

    // Build an expression for a new $match stage.
    BSONObjBuilder firstBob;
    newExpr.first->serialize(&firstBob);

    intrusive_ptr<DocumentSource> firstMatch(new DocumentSourceMatch(firstBob.obj(), pExpCtx));

    // This $match stage is still needed, so update the MatchExpression as needed.
    BSONObjBuilder secondBob;
    newExpr.second->serialize(&secondBob);

    intrusive_ptr<DocumentSource> secondMatch(new DocumentSourceMatch(secondBob.obj(), pExpCtx));

    return {firstMatch, secondMatch};
}

boost::intrusive_ptr<DocumentSourceMatch> DocumentSourceMatch::descendMatchOnPath(
    MatchExpression* matchExpr,
    const std::string& descendOn,
    intrusive_ptr<ExpressionContext> expCtx) {
    expression::mapOver(matchExpr, [&descendOn](MatchExpression* node, std::string path) -> void {
        // Cannot call this method on a $match including a $elemMatch.
        invariant(node->matchType() != MatchExpression::ELEM_MATCH_OBJECT &&
                  node->matchType() != MatchExpression::ELEM_MATCH_VALUE);
        // Logical nodes do not have a path, but both 'leaf' and 'array' nodes
        // do.
        if (node->isLogical()) {
            return;
        }

        auto leafPath = node->path();
        invariant(expression::isPathPrefixOf(descendOn, leafPath));

        auto newPath = leafPath.substr(descendOn.size() + 1);
        if (node->isLeaf() && node->matchType() != MatchExpression::TYPE_OPERATOR &&
            node->matchType() != MatchExpression::WHERE) {
            auto leafNode = static_cast<LeafMatchExpression*>(node);
            leafNode->setPath(newPath);
        } else if (node->isArray()) {
            auto arrayNode = static_cast<ArrayMatchingMatchExpression*>(node);
            arrayNode->setPath(newPath);
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

intrusive_ptr<DocumentSource> DocumentSourceMatch::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15959, "the match filter must be an expression in an object", elem.type() == Object);

    uassertNoDisallowedClauses(elem.Obj());

    return new DocumentSourceMatch(elem.Obj(), pExpCtx);
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

void DocumentSourceMatch::doInjectExpressionContext() {
    _expression->setCollator(pExpCtx->getCollator());
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
