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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * Comparator for MatchExpression nodes.  Returns an integer less than, equal to, or greater
 * than zero if 'lhs' is less than, equal to, or greater than 'rhs', respectively.
 *
 * Sorts by:
 * 1) operator type (MatchExpression::MatchType)
 * 2) path name (MatchExpression::path())
 * 3) sort order of children
 * 4) number of children (MatchExpression::numChildren())
 *
 * The third item is needed to ensure that match expression trees which should have the same
 * cache key always sort the same way. If you're wondering when the tuple (operator type, path
 * name) could ever be equal, consider this query:
 *
 * {$and:[{$or:[{a:1},{a:2}]},{$or:[{a:1},{b:2}]}]}
 *
 * The two OR nodes would compare as equal in this case were it not for tuple item #3 (sort
 * order of children).
 */
int matchExpressionComparator(const MatchExpression* lhs, const MatchExpression* rhs) {
    MatchExpression::MatchType lhsMatchType = lhs->matchType();
    MatchExpression::MatchType rhsMatchType = rhs->matchType();
    if (lhsMatchType != rhsMatchType) {
        return lhsMatchType < rhsMatchType ? -1 : 1;
    }

    StringData lhsPath = lhs->path();
    StringData rhsPath = rhs->path();
    int pathsCompare = lhsPath.compare(rhsPath);
    if (pathsCompare != 0) {
        return pathsCompare;
    }

    const size_t numChildren = std::min(lhs->numChildren(), rhs->numChildren());
    for (size_t childIdx = 0; childIdx < numChildren; ++childIdx) {
        int childCompare =
            matchExpressionComparator(lhs->getChild(childIdx), rhs->getChild(childIdx));
        if (childCompare != 0) {
            return childCompare;
        }
    }

    if (lhs->numChildren() != rhs->numChildren()) {
        return lhs->numChildren() < rhs->numChildren() ? -1 : 1;
    }

    // They're equal!
    return 0;
}

bool matchExpressionLessThan(const MatchExpression* lhs, const MatchExpression* rhs) {
    return matchExpressionComparator(lhs, rhs) < 0;
}

bool parsingCanProduceNoopMatchNodes(const ExtensionsCallback& extensionsCallback,
                                     MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    return extensionsCallback.hasNoopExtensions() &&
        (allowedFeatures & MatchExpressionParser::AllowedFeatures::kText ||
         allowedFeatures & MatchExpressionParser::AllowedFeatures::kJavascript);
}

}  // namespace

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx,
    const QueryMessage& qm,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    // Make QueryRequest.
    auto qrStatus = QueryRequest::fromLegacyQueryMessage(qm);
    if (!qrStatus.isOK()) {
        return qrStatus.getStatus();
    }

    return CanonicalQuery::canonicalize(
        opCtx, std::move(qrStatus.getValue()), expCtx, extensionsCallback, allowedFeatures);
}

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx,
    std::unique_ptr<QueryRequest> qr,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    auto qrStatus = qr->validate();
    if (!qrStatus.isOK()) {
        return qrStatus;
    }

    std::unique_ptr<CollatorInterface> collator;
    if (!qr->getCollation().isEmpty()) {
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(qr->getCollation());
        if (!statusWithCollator.isOK()) {
            return statusWithCollator.getStatus();
        }
        collator = std::move(statusWithCollator.getValue());
    }

    // Make MatchExpression.
    boost::intrusive_ptr<ExpressionContext> newExpCtx;
    if (!expCtx.get()) {
        newExpCtx.reset(new ExpressionContext(opCtx, collator.get(), qr->getRuntimeConstants()));
    } else {
        newExpCtx = expCtx;
        invariant(CollatorInterface::collatorsMatch(collator.get(), expCtx->getCollator()));
    }

    StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
        qr->getFilter(), newExpCtx, extensionsCallback, allowedFeatures);
    if (!statusWithMatcher.isOK()) {
        return statusWithMatcher.getStatus();
    }

    // Stop counting match expressions after they have been parsed to exclude expressions created
    // during optimization and other processing steps.
    newExpCtx->stopExpressionCounters();

    std::unique_ptr<MatchExpression> me = std::move(statusWithMatcher.getValue());

    // Make the CQ we'll hopefully return.
    std::unique_ptr<CanonicalQuery> cq(new CanonicalQuery());

    Status initStatus =
        cq->init(opCtx,
                 std::move(newExpCtx),
                 std::move(qr),
                 parsingCanProduceNoopMatchNodes(extensionsCallback, allowedFeatures),
                 std::move(me),
                 std::move(collator));

    if (!initStatus.isOK()) {
        return initStatus;
    }
    return std::move(cq);
}

// static
StatusWith<std::unique_ptr<CanonicalQuery>> CanonicalQuery::canonicalize(
    OperationContext* opCtx, const CanonicalQuery& baseQuery, MatchExpression* root) {
    auto qr = stdx::make_unique<QueryRequest>(baseQuery.nss());
    BSONObjBuilder builder;
    root->serialize(&builder, true);
    qr->setFilter(builder.obj());
    qr->setProj(baseQuery.getQueryRequest().getProj());
    qr->setSort(baseQuery.getQueryRequest().getSort());
    qr->setCollation(baseQuery.getQueryRequest().getCollation());
    qr->setExplain(baseQuery.getQueryRequest().isExplain());
    auto qrStatus = qr->validate();
    if (!qrStatus.isOK()) {
        return qrStatus;
    }

    std::unique_ptr<CollatorInterface> collator;
    if (baseQuery.getCollator()) {
        collator = baseQuery.getCollator()->clone();
    }

    // Make the CQ we'll hopefully return.
    std::unique_ptr<CanonicalQuery> cq(new CanonicalQuery());
    Status initStatus = cq->init(opCtx,
                                 nullptr,  // no expression context
                                 std::move(qr),
                                 baseQuery.canHaveNoopMatchNodes(),
                                 root->shallowClone(),
                                 std::move(collator));

    if (!initStatus.isOK()) {
        return initStatus;
    }
    return std::move(cq);
}

Status CanonicalQuery::init(OperationContext* opCtx,
                            boost::intrusive_ptr<ExpressionContext> expCtx,
                            std::unique_ptr<QueryRequest> qr,
                            bool canHaveNoopMatchNodes,
                            std::unique_ptr<MatchExpression> root,
                            std::unique_ptr<CollatorInterface> collator) {
    _expCtx = expCtx;
    _qr = std::move(qr);
    _collator = std::move(collator);

    _canHaveNoopMatchNodes = canHaveNoopMatchNodes;

    // Validate, normalize, and sort tree.
    auto validStatus = isValid(root.get(), *_qr);
    if (!validStatus.isOK()) {
        return validStatus;
    }
    _root = MatchExpression::optimize(std::move(root));
    sortTree(_root.get());
    // The tree must always be valid after normalization.
    dassert(isValid(_root.get(), *_qr).isOK());
    if (auto status = isValidNormalized(_root.get()); !status.isOK()) {
        return status;
    }

    // Validate the projection if there is one.
    if (!_qr->getProj().isEmpty()) {
        ParsedProjection* pp;
        Status projStatus = ParsedProjection::make(opCtx, _qr->getProj(), _root.get(), &pp);
        if (!projStatus.isOK()) {
            return projStatus;
        }
        _proj.reset(pp);
    }

    if (_proj && _proj->wantSortKey() && _qr->getSort().isEmpty()) {
        return Status(ErrorCodes::BadValue, "cannot use sortKey $meta projection without a sort");
    }

    return Status::OK();
}

void CanonicalQuery::setCollator(std::unique_ptr<CollatorInterface> collator) {
    _collator = std::move(collator);

    // The collator associated with the match expression tree is now invalid, since we have reset
    // the object owned by '_collator'. We must associate the match expression tree with the new
    // value of '_collator'.
    _root->setCollator(_collator.get());
}

// static
bool CanonicalQuery::isSimpleIdQuery(const BSONObj& query) {
    bool hasID = false;

    BSONObjIterator it(query);
    while (it.more()) {
        BSONElement elt = it.next();
        if (elt.fieldNameStringData() == "_id") {
            // Verify that the query on _id is a simple equality.
            hasID = true;

            if (elt.type() == Object) {
                // If the value is an object, it can't have a query operator
                // (must be a literal object match).
                if (elt.Obj().firstElementFieldName()[0] == '$') {
                    return false;
                }
            } else if (!Indexability::isExactBoundsGenerating(elt)) {
                // The _id fild cannot be something like { _id : { $gt : ...
                // But it can be BinData.
                return false;
            }
        } else {
            return false;
        }
    }

    return hasID;
}

// static
void CanonicalQuery::sortTree(MatchExpression* tree) {
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        sortTree(tree->getChild(i));
    }
    std::vector<MatchExpression*>* children = tree->getChildVector();
    if (NULL != children) {
        std::sort(children->begin(), children->end(), matchExpressionLessThan);
    }
}

// static
size_t CanonicalQuery::countNodes(const MatchExpression* root, MatchExpression::MatchType type) {
    size_t sum = 0;
    if (type == root->matchType()) {
        sum = 1;
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        sum += countNodes(root->getChild(i), type);
    }
    return sum;
}

/**
 * Does 'root' have a subtree of type 'subtreeType' with a node of type 'childType' inside?
 */
bool hasNodeInSubtree(const MatchExpression* root,
                      MatchExpression::MatchType childType,
                      MatchExpression::MatchType subtreeType) {
    if (subtreeType == root->matchType()) {
        return QueryPlannerCommon::hasNode(root, childType);
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        if (hasNodeInSubtree(root->getChild(i), childType, subtreeType)) {
            return true;
        }
    }
    return false;
}

// static
Status CanonicalQuery::isValid(const MatchExpression* root, const QueryRequest& parsed) {
    // There can only be one TEXT.  If there is a TEXT, it cannot appear inside a NOR.
    //
    // Note that the query grammar (as enforced by the MatchExpression parser) forbids TEXT
    // inside of value-expression clauses like NOT, so we don't check those here.
    size_t numText = countNodes(root, MatchExpression::TEXT);
    if (numText > 1) {
        return Status(ErrorCodes::BadValue, "Too many text expressions");
    } else if (1 == numText) {
        if (hasNodeInSubtree(root, MatchExpression::TEXT, MatchExpression::NOR)) {
            return Status(ErrorCodes::BadValue, "text expression not allowed in nor");
        }
    }

    // There can only be one NEAR.  If there is a NEAR, it must be either the root or the root
    // must be an AND and its child must be a NEAR.
    size_t numGeoNear = countNodes(root, MatchExpression::GEO_NEAR);
    if (numGeoNear > 1) {
        return Status(ErrorCodes::BadValue, "Too many geoNear expressions");
    } else if (1 == numGeoNear) {
        // Do nothing, we will perform extra checks in CanonicalQuery::isValidNormalized.
    }

    // NEAR cannot have a $natural sort or $natural hint.
    const BSONObj& sortObj = parsed.getSort();
    BSONElement sortNaturalElt = sortObj["$natural"];
    const BSONObj& hintObj = parsed.getHint();
    BSONElement hintNaturalElt = hintObj["$natural"];
    if (numGeoNear > 0) {
        if (sortNaturalElt) {
            return Status(ErrorCodes::BadValue,
                          "geoNear expression not allowed with $natural sort order");
        }

        if (hintNaturalElt) {
            return Status(ErrorCodes::BadValue,
                          "geoNear expression not allowed with $natural hint");
        }
    }

    // TEXT and NEAR cannot both be in the query.
    if (numText > 0 && numGeoNear > 0) {
        return Status(ErrorCodes::BadValue, "text and geoNear not allowed in same query");
    }

    // TEXT and {$natural: ...} sort order cannot both be in the query.
    if (numText > 0 && sortNaturalElt) {
        return Status(ErrorCodes::BadValue, "text expression not allowed with $natural sort order");
    }

    // TEXT and hint cannot both be in the query.
    if (numText > 0 && !hintObj.isEmpty()) {
        return Status(ErrorCodes::BadValue, "text and hint not allowed in same query");
    }

    // TEXT and tailable are incompatible.
    if (numText > 0 && parsed.isTailable()) {
        return Status(ErrorCodes::BadValue, "text and tailable cursor not allowed in same query");
    }

    // NEAR and tailable are incompatible.
    if (numGeoNear > 0 && parsed.isTailable()) {
        return Status(ErrorCodes::BadValue,
                      "Tailable cursors and geo $near cannot be used together");
    }

    // $natural sort order must agree with hint.
    if (sortNaturalElt) {
        if (!hintObj.isEmpty() && !hintNaturalElt) {
            return Status(ErrorCodes::BadValue, "index hint not allowed with $natural sort order");
        }
        if (hintNaturalElt) {
            if (hintNaturalElt.numberInt() != sortNaturalElt.numberInt()) {
                return Status(ErrorCodes::BadValue,
                              "$natural hint must be in the same direction as $natural sort order");
            }
        }
    }

    return Status::OK();
}

Status CanonicalQuery::isValidNormalized(const MatchExpression* root) {
    if (auto numGeoNear = countNodes(root, MatchExpression::GEO_NEAR); numGeoNear > 0) {
        invariant(numGeoNear == 1);

        auto topLevel = false;
        if (MatchExpression::GEO_NEAR == root->matchType()) {
            topLevel = true;
        } else if (MatchExpression::AND == root->matchType()) {
            for (size_t i = 0; i < root->numChildren(); ++i) {
                if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                    topLevel = true;
                    break;
                }
            }
        }

        if (!topLevel) {
            return Status(ErrorCodes::BadValue, "geoNear must be top-level expr");
        }
    }

    return Status::OK();
}

std::string CanonicalQuery::toString() const {
    str::stream ss;
    ss << "ns=" << _qr->nss().ns();

    if (_qr->getBatchSize()) {
        ss << " batchSize=" << *_qr->getBatchSize();
    }

    if (_qr->getLimit()) {
        ss << " limit=" << *_qr->getLimit();
    }

    if (_qr->getSkip()) {
        ss << " skip=" << *_qr->getSkip();
    }

    if (_qr->getNToReturn()) {
        ss << " ntoreturn=" << *_qr->getNToReturn() << '\n';
    }

    // The expression tree puts an endl on for us.
    ss << "Tree: " << _root->debugString();
    ss << "Sort: " << _qr->getSort().toString() << '\n';
    ss << "Proj: " << _qr->getProj().toString() << '\n';
    if (!_qr->getCollation().isEmpty()) {
        ss << "Collation: " << _qr->getCollation().toString() << '\n';
    }
    return ss;
}

std::string CanonicalQuery::toStringShort() const {
    str::stream ss;
    ss << "ns: " << _qr->nss().ns() << " query: " << _qr->getFilter().toString()
       << " sort: " << _qr->getSort().toString() << " projection: " << _qr->getProj().toString();

    if (!_qr->getCollation().isEmpty()) {
        ss << " collation: " << _qr->getCollation().toString();
    }

    if (_qr->getBatchSize()) {
        ss << " batchSize: " << *_qr->getBatchSize();
    }

    if (_qr->getLimit()) {
        ss << " limit: " << *_qr->getLimit();
    }

    if (_qr->getSkip()) {
        ss << " skip: " << *_qr->getSkip();
    }

    if (_qr->getNToReturn()) {
        ss << " ntoreturn=" << *_qr->getNToReturn();
    }

    return ss;
}

CanonicalQuery::QueryShapeString CanonicalQuery::encodeKey() const {
    return canonical_query_encoder::encode(*this);
}

}  // namespace mongo
