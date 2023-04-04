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


#include "mongo/db/query/planner_ixselect.h"

#include <vector>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

namespace wcp = ::mongo::wildcard_planning;

// Can't index negations of {$eq: <Array>}, {$lt: <Array>}, {$gt: <Array>}, {$lte: <Array>}, {$gte:
// <Array>}, or {$in: [<Array>, ...]}. Note that we could use the index in principle, though we
// would need to generate special bounds.
bool isComparisonWithArrayPred(const MatchExpression* me) {
    const auto type = me->matchType();
    if (type == MatchExpression::EQ || type == MatchExpression::LT || type == MatchExpression::GT ||
        type == MatchExpression::LTE || type == MatchExpression::GTE) {
        return static_cast<const ComparisonMatchExpression*>(me)->getData().type() ==
            BSONType::Array;
    } else if (type == MatchExpression::MATCH_IN) {
        const auto& equalities = static_cast<const InMatchExpression*>(me)->getEqualities();
        return std::any_of(equalities.begin(), equalities.end(), [](BSONElement elt) {
            return elt.type() == BSONType::Array;
        });
    }
    return false;
}

std::size_t numPathComponents(StringData path) {
    return FieldRef{path}.numParts();
}

bool canUseWildcardIndex(BSONElement elt, MatchExpression::MatchType matchType) {
    if (elt.type() == BSONType::Object) {
        // $** indices break nested objects into separate keys, which means we can't naturally
        // support comparison-to-object predicates. However, there is an exception: empty objects
        // are indexed like regular leaf values. This means that equality-to-empty-object can be
        // supported.
        //
        // Due to type bracketing, $lte:{} and $eq:{} are semantically equivalent.
        return elt.embeddedObject().isEmpty() &&
            (matchType == MatchExpression::EQ || matchType == MatchExpression::LTE);
    }

    if (elt.type() == BSONType::Array) {
        // We only support equality to empty array.
        return elt.embeddedObject().isEmpty() && matchType == MatchExpression::EQ;
    }

    return true;
}

}  // namespace

bool QueryPlannerIXSelect::notEqualsNullCanUseIndex(const IndexEntry& index,
                                                    const BSONElement& keyPatternElt,
                                                    std::size_t keyPatternIndex,
                                                    const ElemMatchContext& elemMatchContext) {
    // It is safe to use a non-multikey index for not equals null queries.
    if (!index.multikey && index.multikeyPaths.empty()) {
        // This is an old index without multikey path metadata.
        return true;
    }
    if (!index.multikeyPaths.empty() && index.multikeyPaths[keyPatternIndex].empty()) {
        // This part of the index has no multikey components, it is always safe to use the index.
        return true;
    }

    // At least one component is multikey. In most circumstances, we can't index the negation of EQ
    // with a value of null if the index is multikey on one of the components of the path.
    //
    // This is quite subtle, and due to the semantics of null matching. For example, if the query is
    // {a: {$ne: null}}, you might expect us to build index bounds of [MinKey, undefined) and
    // (null, MaxKey] (or similar) on an 'a' index. However, with this query the document {a: []}
    // should match (because it does not match {a: null}), but will have an index key of undefined.
    // Similarly, the document {a: [null, null]} matches the query {'a.b': {$ne: null}}, but would
    // have an index key of null in an index on 'a.b'. Since it's possible for a key of undefined to
    // be included in the results and also possible for a value of null to be included, there are no
    // restrictions on the bounds of the index for such a predicate. Further, such an index could
    // not be used for covering, so would not provide any help to the query.
    //
    // There are two exceptions to this rule, both having to do with $elemMatch, see below.
    auto* parentElemMatch = elemMatchContext.innermostParentElemMatch;
    if (!parentElemMatch) {
        // See above, if there's no $elemMatch we can't use the index.
        return false;
    }

    if (MatchExpression::ELEM_MATCH_VALUE == parentElemMatch->matchType()) {
        // If this $ne clause is within a $elemMatch *value*, the semantics of $elemMatch guarantee
        // that no matching values will be null or undefined, even if the index is multikey.
        //
        // For example, the document {a: []} does *not* match the query {a: {$elemMatch: {$ne:
        // null}} because there was no element within the array that matched. While the document {a:
        // [[]]} *does* match that query, the index entry for that document would be [], not null or
        // undefined.
        return true;
    } else {
        invariant(MatchExpression::ELEM_MATCH_OBJECT == parentElemMatch->matchType());
        if (index.multikeyPaths.empty()) {
            // The index has no path-level multikey metadata, we can't do the analysis below so have
            // to be defensive.
            return false;
        }

        // This $ne clause is within an $elemMatch *object*. We can safely use the index so long as
        // there are no multikey paths below the $elemMatch.
        //
        // For example, take the query {"a.b": {$elemMatch: {"c.d": {$ne: null}}}}. We can use an
        // "a.b.c.d" index if _only_ "a" and/or "a.b" is/are multikey, because there will be no
        // array traversal for the "c.d" part of the query. If "a.b.c" or "a.b.c.d" are multikey,
        // we cannot use this index. As an example of what would go wrong, suppose the collection
        // contained the document {a: {b: {c: []}}}. The "a.b.c.d" index key for that document
        // would be null, and so would be excluded from the index bounds. However, that document
        // should match the query.
        const std::size_t firstComponentAfterElemMatch =
            numPathComponents(elemMatchContext.fullPathToParentElemMatch);
        for (auto&& multikeyComponent : index.multikeyPaths[keyPatternIndex]) {
            if (multikeyComponent >= firstComponentAfterElemMatch) {
                return false;
            }
        }

        // The index was multikey, but only on paths that came before the $elemMatch, so we can
        // safely use the index without having to worry about implicitly traversing arrays.
        return true;
    }
}

bool QueryPlannerIXSelect::canUseIndexForNin(const InMatchExpression* ime) {
    const std::vector<BSONElement>& inList = ime->getEqualities();
    auto containsNull = [](const BSONElement& elt) {
        return elt.type() == jstNULL;
    };
    auto containsEmptyArray = [](const BSONElement& elt) {
        return elt.type() == Array && elt.embeddedObject().isEmpty();
    };
    return !ime->hasRegex() && inList.size() == 2 &&
        std::any_of(inList.begin(), inList.end(), containsNull) &&
        std::any_of(inList.begin(), inList.end(), containsEmptyArray);
}

/**
 * 2d indices don't handle wrapping so we can't use them for queries that wrap.
 */
static bool twoDWontWrap(const Circle& circle, const IndexEntry& index) {
    auto conv = GeoHashConverter::createFromDoc(index.infoObj);
    uassertStatusOK(conv.getStatus());  // We validated the parameters when creating the index.

    // FYI: old code used flat not spherical error.
    double yscandist = rad2deg(circle.radius) + conv.getValue()->getErrorSphere();
    double xscandist = computeXScanDistance(circle.center.y, yscandist);
    bool ret = circle.center.x + xscandist < 180 && circle.center.x - xscandist > -180 &&
        circle.center.y + yscandist < 90 && circle.center.y - yscandist > -90;
    return ret;
}

// Checks whether 'node' contains any comparison to an element of type 'type'. Nested objects and
// arrays are not checked recursively. We assume 'node' is bounds-generating or is a recursive child
// of a bounds-generating node, i.e. it does not contain AND, OR, ELEM_MATCH_OBJECT, or NOR.
static bool boundsGeneratingNodeContainsComparisonToType(MatchExpression* node, BSONType type) {
    invariant(node->matchType() != MatchExpression::AND &&
              node->matchType() != MatchExpression::OR &&
              node->matchType() != MatchExpression::NOR &&
              node->matchType() != MatchExpression::ELEM_MATCH_OBJECT);

    if (const auto* comparisonExpr = dynamic_cast<const ComparisonMatchExpressionBase*>(node)) {
        return comparisonExpr->getData().type() == type;
    }

    if (node->matchType() == MatchExpression::MATCH_IN) {
        const InMatchExpression* expr = static_cast<const InMatchExpression*>(node);
        for (const auto& equality : expr->getEqualities()) {
            if (equality.type() == type) {
                return true;
            }
        }
        return false;
    }

    if (node->matchType() == MatchExpression::NOT) {
        invariant(node->numChildren() == 1U);
        return boundsGeneratingNodeContainsComparisonToType(node->getChild(0), type);
    }

    if (node->matchType() == MatchExpression::ELEM_MATCH_VALUE) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            if (boundsGeneratingNodeContainsComparisonToType(node->getChild(i), type)) {
                return true;
            }
        }
        return false;
    }

    return false;
}

// static
void QueryPlannerIXSelect::getFields(const MatchExpression* node,
                                     string prefix,
                                     RelevantFieldIndexMap* out) {
    // Do not traverse tree beyond a NOR negation node
    MatchExpression::MatchType exprtype = node->matchType();
    if (exprtype == MatchExpression::NOR) {
        return;
    }

    // Leaf nodes with a path and some array operators.
    if (Indexability::nodeCanUseIndexOnOwnField(node)) {
        bool supportSparse = Indexability::nodeSupportedBySparseIndex(node);
        (*out)[prefix + node->path().toString()] = {supportSparse};
    } else if (Indexability::arrayUsesIndexOnChildren(node) && !node->path().empty()) {
        // If the array uses an index on its children, it's something like
        // {foo : {$elemMatch: {bar: 1}}}, in which case the predicate is really over foo.bar.
        // Note we skip empty path components since they are not allowed in index key patterns.
        prefix += node->path().toString() + ".";

        for (size_t i = 0; i < node->numChildren(); ++i) {
            getFields(node->getChild(i), prefix, out);
        }
    } else if (node->getCategory() == MatchExpression::MatchCategory::kLogical) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            getFields(node->getChild(i), prefix, out);
        }
    }
}

void QueryPlannerIXSelect::getFields(const MatchExpression* node, RelevantFieldIndexMap* out) {
    getFields(node, "", out);
}

// static
std::vector<IndexEntry> QueryPlannerIXSelect::findIndexesByHint(
    const BSONObj& hintedIndex, const std::vector<IndexEntry>& allIndices) {
    std::vector<IndexEntry> out;
    BSONElement firstHintElt = hintedIndex.firstElement();
    if (firstHintElt.fieldNameStringData() == "$hint"_sd &&
        firstHintElt.type() == BSONType::String) {
        auto hintName = firstHintElt.valueStringData();
        for (auto&& entry : allIndices) {
            if (entry.identifier.catalogName == hintName) {
                LOGV2_DEBUG(20952,
                            5,
                            "Hint by name specified, restricting indices",
                            "name"_attr = entry.identifier.catalogName,
                            "keyPattern"_attr = entry.keyPattern);
                out.push_back(entry);
            }
        }
    } else {
        for (auto&& entry : allIndices) {
            if (SimpleBSONObjComparator::kInstance.evaluate(entry.keyPattern == hintedIndex)) {
                LOGV2_DEBUG(20953,
                            5,
                            "Hint specified, restricting indices",
                            "name"_attr = entry.identifier.catalogName,
                            "keyPattern"_attr = entry.keyPattern);
                out.push_back(entry);
            }
        }
    }

    return out;
}

// static
std::vector<IndexEntry> QueryPlannerIXSelect::findRelevantIndices(
    const RelevantFieldIndexMap& fields, const std::vector<IndexEntry>& allIndices) {

    std::vector<IndexEntry> out;
    for (auto&& index : allIndices) {
        BSONObjIterator it(index.keyPattern);
        BSONElement elt = it.next();
        const std::string fieldName = elt.fieldNameStringData().toString();

        // If the index is non-sparse we can use the field regardless its sparsity, otherwise we
        // should find the field that can be answered by a sparse index.
        if (fields.contains(fieldName) &&
            (!index.sparse || fields.find(fieldName)->second.isSparse)) {
            out.push_back(index);
        }
    }

    return out;
}

std::vector<IndexEntry> QueryPlannerIXSelect::expandIndexes(const RelevantFieldIndexMap& fields,
                                                            std::vector<IndexEntry> relevantIndices,
                                                            bool indexHinted) {
    std::vector<IndexEntry> out;
    // Filter out fields that cannot be answered by any sparse index. We know wildcard indexes are
    // sparse, so we don't want to expand the wildcard index based on such fields.
    stdx::unordered_set<std::string> sparseIncompatibleFields;
    for (auto&& [fieldName, idxProperty] : fields) {
        if (idxProperty.isSparse || indexHinted) {
            sparseIncompatibleFields.insert(fieldName);
        }
    }
    for (auto&& entry : relevantIndices) {
        if (entry.type == IndexType::INDEX_WILDCARD) {
            wcp::expandWildcardIndexEntry(entry, sparseIncompatibleFields, &out);
        } else {
            out.push_back(std::move(entry));
        }
    }

    // As a post-condition, all expanded index entries must _not_ contain a multikey path set.
    // Multikey metadata is converted to the fixed-size vector representation as part of expanding
    // indexes.
    for (auto&& indexEntry : out) {
        invariant(indexEntry.multikeyPathSet.empty());
    }

    return out;
}

// static
bool QueryPlannerIXSelect::_compatible(const BSONElement& keyPatternElt,
                                       const IndexEntry& index,
                                       std::size_t keyPatternIdx,
                                       MatchExpression* node,
                                       StringData fullPathToNode,
                                       const CollatorInterface* collator,
                                       const ElemMatchContext& elemMatchContext) {
    if ((boundsGeneratingNodeContainsComparisonToType(node, BSONType::String) ||
         boundsGeneratingNodeContainsComparisonToType(node, BSONType::Array) ||
         boundsGeneratingNodeContainsComparisonToType(node, BSONType::Object)) &&
        !CollatorInterface::collatorsMatch(collator, index.collator)) {
        return false;
    }

    if (index.type == IndexType::INDEX_WILDCARD) {
        // Fields after "$_path" of a compound wildcard index should not be used to answer any
        // query, because wildcard IndexEntry with reserved field, "$_path", present is used only
        // to answer query on non-wildcard prefix.
        size_t idx = 0;
        for (auto&& elt : index.keyPattern) {
            if (elt.fieldNameStringData() == "$_path") {
                return false;
            }
            if (idx == keyPatternIdx) {
                break;
            }
            idx++;
        }

        // If this IndexEntry is considered relevant to a regular field of a compound wildcard
        // index, the IndexEntry must not have a specific expanded field for the wildcard field.
        // Instead, the key pattern should contain a reserved "$_path" field.
        if (keyPatternIdx < index.wildcardFieldPos && !index.keyPattern.hasField("$_path")) {
            return false;
        }
    }

    // Historically one could create indices with any particular value for the index spec,
    // including values that now indicate a special index.  As such we have to make sure the
    // index type wasn't overridden before we pay attention to the string in the index key
    // pattern element.
    //
    // e.g. long ago we could have created an index {a: "2dsphere"} and it would
    // be treated as a btree index by an ancient version of MongoDB.  To try to run
    // 2dsphere queries over it would be folly.
    string indexedFieldType;
    if (String != keyPatternElt.type() || (INDEX_BTREE == index.type)) {
        indexedFieldType = "";
    } else {
        indexedFieldType = keyPatternElt.String();
    }

    const bool isChildOfElemMatchValue = elemMatchContext.innermostParentElemMatch &&
        elemMatchContext.innermostParentElemMatch->matchType() == MatchExpression::ELEM_MATCH_VALUE;

    // We know keyPatternElt.fieldname() == node->path().
    MatchExpression::MatchType exprtype = node->matchType();

    if (ComparisonMatchExpressionBase::isInternalExprComparison(exprtype) &&
        index.pathHasMultikeyComponent(keyPatternElt.fieldNameStringData())) {
        // Expression language comparisons cannot be indexed if the field path has multikey
        // components.
        return false;
    }

    if (indexedFieldType.empty()) {
        // We can't use a sparse index for certain match expressions.
        if (index.sparse && !nodeIsSupportedBySparseIndex(node, isChildOfElemMatchValue)) {
            return false;
        }

        // We can't use a btree-indexed field for geo expressions.
        if (exprtype == MatchExpression::GEO || exprtype == MatchExpression::GEO_NEAR ||
            exprtype == MatchExpression::INTERNAL_BUCKET_GEO_WITHIN) {
            return false;
        }

        // There are restrictions on when we can use the index if the expression is a NOT.
        if (exprtype == MatchExpression::NOT) {
            // Don't allow indexed NOT on special index types such as geo or text indices. There are
            // two exceptions to this rule:
            // - Wildcard indexes can answer {$ne: null} queries. We allow wildcard indexes to pass
            //   the test here because we subsequently enforce that {$ne:null} is the only accepted
            //   negation predicate for sparse indexes, and wildcard indexes are always sparse.
            // - Any non-hashed field in a compound hashed index can answer negated predicates. We
            //   have already determined that the specific field under consideration is not hashed,
            //   so we can safely permit a hashed index to pass the test below.
            //
            // TODO: SERVER-30994 should remove this check entirely and allow $not on the
            // 'non-special' fields of non-btree indices (e.g. {a: 1, geo: "2dsphere"}).
            if (INDEX_BTREE != index.type && INDEX_WILDCARD != index.type &&
                INDEX_HASHED != index.type && !isChildOfElemMatchValue) {
                return false;
            }

            // The type being INDEX_WILDCARD implies that the index is sparse.
            invariant(index.sparse || index.type != INDEX_WILDCARD);

            const auto* child = node->getChild(0);
            const MatchExpression::MatchType childtype = child->matchType();

            // Can't index negations of MOD, REGEX, TYPE_OPERATOR, or ELEM_MATCH_VALUE.
            if (MatchExpression::REGEX == childtype || MatchExpression::MOD == childtype ||
                MatchExpression::TYPE_OPERATOR == childtype ||
                MatchExpression::ELEM_MATCH_VALUE == childtype) {
                return false;
            }

            // $gt and $lt to MinKey/MaxKey must build inexact bounds if the index is multikey and
            // therefore cannot be inverted safely in a $not.
            if (index.multikey && (child->isGTMinKey() || child->isLTMaxKey())) {
                return false;
            }

            // Most of the time we can't use a multikey index for a $ne: null query, however there
            // are a few exceptions around $elemMatch.
            const bool isNotEqualsNull = isQueryNegatingEqualToNull(node);
            const bool canUseIndexForNeNull =
                notEqualsNullCanUseIndex(index, keyPatternElt, keyPatternIdx, elemMatchContext);
            if (isNotEqualsNull && !canUseIndexForNeNull) {
                return false;
            }

            // If it's a negated $in, it can't have any REGEX's inside.
            if (MatchExpression::MATCH_IN == childtype) {
                InMatchExpression* ime = static_cast<InMatchExpression*>(node->getChild(0));

                if (canUseIndexForNin(ime)) {
                    // This is a case that we know is supported.
                    return true;
                }

                if (!ime->getRegexes().empty()) {
                    return false;
                }

                // If we can't use the index for $ne to null, then we cannot use it for the
                // case {$nin: [null, <...>]}.
                if (!canUseIndexForNeNull && ime->hasNull()) {
                    return false;
                }
            }

            // Comparisons with arrays have strange enough semantics that inverting the bounds
            // within a $not has many complex special cases. We avoid indexing these queries, even
            // though it is sometimes possible to build useful bounds.
            if (isComparisonWithArrayPred(child)) {
                return false;
            }
        }

        // If this is an $elemMatch value, make sure _all_ of the children can use the index.
        if (node->matchType() == MatchExpression::ELEM_MATCH_VALUE) {
            ElemMatchContext newContext;
            newContext.fullPathToParentElemMatch = fullPathToNode;
            newContext.innermostParentElemMatch = static_cast<ElemMatchValueMatchExpression*>(node);

            FieldRef path(fullPathToNode);
            // If the index path has at least two components, and the last component of the path is
            // numeric, this component could be an array index because the preceding path component
            // may contain an array. Currently it is not known whether the preceding path component
            // could be an array because indexes which positionally index array elements are not
            // considered multikey.
            if (path.numParts() > 1 && path.isNumericPathComponentStrict(path.numParts() - 1)) {
                return false;
            }

            auto&& children = node->getChildVector();
            if (!std::all_of(children->begin(), children->end(), [&](auto&& child) {
                    const auto newPath = fullPathToNode.toString() + child->path();
                    return _compatible(keyPatternElt,
                                       index,
                                       keyPatternIdx,
                                       child.get(),
                                       newPath,
                                       collator,
                                       newContext);
                })) {
                return false;
            }
        }

        if (index.type == IndexType::INDEX_WILDCARD && !nodeIsSupportedByWildcardIndex(node)) {
            return false;
        }

        // We can only index EQ using text indices.  This is an artificial limitation imposed by
        // FTSSpec::getIndexPrefix() which will fail if there is not an EQ predicate on each
        // index prefix field of the text index.
        //
        // Example for key pattern {a: 1, b: "text"}:
        // - Allowed: node = {a: 7}
        // - Not allowed: node = {a: {$gt: 7}}

        if (INDEX_TEXT != index.type) {
            return true;
        }

        // If we're here we know it's a text index.  Equalities are OK anywhere in a text index.
        if (MatchExpression::EQ == exprtype) {
            return true;
        }

        // Not-equalities can only go in a suffix field of an index kp.  We look through the key
        // pattern to see if the field we're looking at now appears as a prefix.  If so, we
        // can't use this index for it.
        for (auto&& elt : index.keyPattern) {
            // We hit the dividing mark between prefix and suffix, so whatever field we're
            // looking at is a suffix, since it appears *after* the dividing mark between the
            // two.  As such, we can use the index.
            if (String == elt.type()) {
                return true;
            }

            // If we're here, we're still looking at prefix elements.  We know that exprtype
            // isn't EQ so we can't use this index.
            if (node->path() == elt.fieldNameStringData()) {
                return false;
            }
        }

        // Text index implies there is a separator implies we will always hit the 'return true'
        // above.
        MONGO_UNREACHABLE;
    } else if (IndexNames::HASHED == indexedFieldType) {
        if (index.sparse && !nodeIsSupportedBySparseIndex(node, isChildOfElemMatchValue)) {
            return false;
        }

        return nodeIsSupportedByHashedIndex(node);
    } else if (IndexNames::GEO_2DSPHERE == indexedFieldType) {
        if (exprtype == MatchExpression::GEO) {
            // within or intersect.
            GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
            const GeoExpression& gq = gme->getGeoExpression();
            const GeometryContainer& gc = gq.getGeometry();
            return gc.hasS2Region();
        } else if (exprtype == MatchExpression::GEO_NEAR) {
            GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
            // Make sure the near query is compatible with 2dsphere.
            return gnme->getData().centroid->crs == SPHERE;
        }
        return false;
    } else if (IndexNames::GEO_2DSPHERE_BUCKET == indexedFieldType) {
        if (exprtype == MatchExpression::INTERNAL_BUCKET_GEO_WITHIN) {
            const InternalBucketGeoWithinMatchExpression* ibgwme =
                static_cast<InternalBucketGeoWithinMatchExpression*>(node);
            auto gc = ibgwme->getGeoContainer();
            return gc.hasS2Region();
        }
        return false;
    } else if (IndexNames::GEO_2D == indexedFieldType) {
        if (exprtype == MatchExpression::GEO_NEAR) {
            GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
            // Make sure the near query is compatible with 2d index
            return gnme->getData().centroid->crs == FLAT || !gnme->getData().isWrappingQuery;
        } else if (exprtype == MatchExpression::GEO) {
            // 2d only supports within.
            GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
            const GeoExpression& gq = gme->getGeoExpression();
            if (GeoExpression::WITHIN != gq.getPred()) {
                return false;
            }

            const GeometryContainer& gc = gq.getGeometry();

            // 2d indices require an R2 covering
            if (gc.hasR2Region()) {
                return true;
            }

            const CapWithCRS* cap = gc.getCapGeometryHack();

            // 2d indices can answer centerSphere queries.
            if (nullptr == cap) {
                return false;
            }

            verify(SPHERE == cap->crs);
            const Circle& circle = cap->circle;

            // No wrapping around the edge of the world is allowed in 2d centerSphere.
            return twoDWontWrap(circle, index);
        }
        return false;
    } else if (IndexNames::TEXT == indexedFieldType) {
        return (exprtype == MatchExpression::TEXT);
    } else if (IndexNames::GEO_HAYSTACK == indexedFieldType) {
        return false;
    } else {
        LOGV2_WARNING(20954,
                      "Unknown indexing for given node and field",
                      "node"_attr = node->debugString(),
                      "field"_attr = keyPatternElt.toString());
        verify(0);
    }
    MONGO_UNREACHABLE;
}

bool QueryPlannerIXSelect::nodeIsSupportedBySparseIndex(const MatchExpression* queryExpr,
                                                        bool isInElemMatch) {
    // The only types of queries which may not be supported by a sparse index are ones which have
    // an equality to null (or an {$exists: false}), because of the language's "null or missing"
    // semantics. {$exists: false} gets translated into a negation query (which a sparse index
    // cannot answer), so this function only needs to check if the query performs an equality to
    // null.

    // Otherwise, we can't use a sparse index for $eq (or $lte, or $gte) with a null element.
    //
    // We can use a sparse index for $_internalExprEq with a null element. Expression language
    // equality-to-null semantics are that only literal nulls match. Sparse indexes contain
    // index keys for literal nulls, but not for missing elements.
    const auto typ = queryExpr->matchType();
    if (typ == MatchExpression::EQ) {
        const auto* queryExprEquality = static_cast<const EqualityMatchExpression*>(queryExpr);
        // Equality to null inside an $elemMatch implies a match on literal 'null'.
        return isInElemMatch || !queryExprEquality->getData().isNull();
    } else if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
        const auto* queryExprIn = static_cast<const InMatchExpression*>(queryExpr);
        // Equality to null inside an $elemMatch implies a match on literal 'null'.
        return isInElemMatch || !queryExprIn->hasNull();
    } else if (queryExpr->matchType() == MatchExpression::NOT) {
        const auto* child = queryExpr->getChild(0);
        const MatchExpression::MatchType childtype = child->matchType();
        const bool isNotEqualsNull =
            (childtype == MatchExpression::EQ &&
             static_cast<const ComparisonMatchExpression*>(child)->getData().type() ==
                 BSONType::jstNULL);

        // Prevent negated predicates from using sparse indices. Doing so would cause us to
        // miss documents which do not contain the indexed fields. The only case where we may
        // use a sparse index for a negation is when the query is {$ne: null}. This is due to
        // the behavior of {$eq: null} matching documents where the field does not exist OR the
        // field is equal to literal null. The negation of {$eq: null} therefore matches
        // documents where the field does exist AND the field is not equal to literal
        // null. Since the field must exist, it is safe to use a sparse index.
        if (!isNotEqualsNull) {
            return false;
        }
    }

    return true;
}

bool QueryPlannerIXSelect::logicalNodeMayBeSupportedByAnIndex(const MatchExpression* queryExpr) {
    return !(queryExpr->matchType() == MatchExpression::NOT &&
             isComparisonWithArrayPred(queryExpr->getChild(0)));
}

bool QueryPlannerIXSelect::nodeIsSupportedByWildcardIndex(const MatchExpression* queryExpr) {
    // Wildcard indexes only store index keys for "leaf" nodes in an object. That is, they do not
    // store keys for nested objects, meaning that any kind of comparison to an object or array
    // cannot be answered by the index (including with a $in).

    if (ComparisonMatchExpression::isComparisonMatchExpression(queryExpr)) {
        const ComparisonMatchExpression* cmpExpr =
            static_cast<const ComparisonMatchExpression*>(queryExpr);

        return canUseWildcardIndex(cmpExpr->getData(), cmpExpr->matchType());
    } else if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
        const auto* queryExprIn = static_cast<const InMatchExpression*>(queryExpr);

        return std::all_of(
            queryExprIn->getEqualities().begin(),
            queryExprIn->getEqualities().end(),
            [](const BSONElement& elt) { return canUseWildcardIndex(elt, MatchExpression::EQ); });
    }

    return true;
}

bool QueryPlannerIXSelect::nodeIsSupportedByHashedIndex(const MatchExpression* queryExpr) {
    // Hashed fields can answer simple equality predicates.
    if (ComparisonMatchExpressionBase::isEquality(queryExpr->matchType())) {
        return true;
    }
    // An $in can be answered so long as its operand contains only simple equalities.
    if (queryExpr->matchType() == MatchExpression::MATCH_IN) {
        const InMatchExpression* expr = static_cast<const InMatchExpression*>(queryExpr);
        return expr->getRegexes().empty();
    }
    // {$exists:false} produces a single point-interval index bound on [null,null].
    if (queryExpr->matchType() == MatchExpression::NOT) {
        return queryExpr->getChild(0)->matchType() == MatchExpression::EXISTS;
    }
    // {$exists:true} can be answered using [MinKey, MaxKey] bounds.
    return (queryExpr->matchType() == MatchExpression::EXISTS);
}

// static
// This is the public method which does not accept an ElemMatchContext.
void QueryPlannerIXSelect::rateIndices(MatchExpression* node,
                                       string prefix,
                                       const vector<IndexEntry>& indices,
                                       const CollatorInterface* collator) {
    return _rateIndices(node, prefix, indices, collator, ElemMatchContext{});
}

// static
void QueryPlannerIXSelect::_rateIndices(MatchExpression* node,
                                        string prefix,
                                        const vector<IndexEntry>& indices,
                                        const CollatorInterface* collator,
                                        const ElemMatchContext& elemMatchCtx) {
    // Do not traverse tree beyond logical NOR node
    MatchExpression::MatchType exprtype = node->matchType();
    if (exprtype == MatchExpression::NOR) {
        return;
    }

    // Every indexable node is tagged even when no compatible index is available.
    if (Indexability::isBoundsGenerating(node)) {
        string fullPath;
        if (MatchExpression::NOT == node->matchType()) {
            fullPath = prefix + node->getChild(0)->path().toString();
        } else {
            fullPath = prefix + node->path().toString();
        }

        verify(nullptr == node->getTag());
        node->setTag(new RelevantTag());
        auto rt = static_cast<RelevantTag*>(node->getTag());
        rt->path = fullPath;

        for (size_t i = 0; i < indices.size(); ++i) {
            const IndexEntry& index = indices[i];
            std::size_t keyPatternIndex = 0;
            for (auto&& keyPatternElt : index.keyPattern) {
                if (keyPatternElt.fieldNameStringData() == fullPath &&
                    _compatible(keyPatternElt,
                                index,
                                keyPatternIndex,
                                node,
                                fullPath,
                                collator,
                                elemMatchCtx)) {
                    if (keyPatternIndex == 0) {
                        rt->first.push_back(i);
                    } else {
                        rt->notFirst.push_back(i);
                    }
                }
                ++keyPatternIndex;
            }
        }

        // If this is a NOT, we have to clone the tag and attach it to the NOT's child.
        if (MatchExpression::NOT == node->matchType()) {
            RelevantTag* childRt = static_cast<RelevantTag*>(rt->clone());
            childRt->path = rt->path;
            node->getChild(0)->setTag(childRt);
        }
    } else if (Indexability::arrayUsesIndexOnChildren(node) && !node->path().empty()) {
        // Note we skip empty path components since they are not allowed in index key patterns.
        const auto newPath = prefix + node->path().toString();
        ElemMatchContext newContext;
        // Note this StringData is unowned and references the string declared on the stack here.
        // This should be fine since we are only ever reading from this in recursive calls as
        // context to help make planning decisions.
        newContext.fullPathToParentElemMatch = newPath;
        newContext.innermostParentElemMatch = static_cast<ElemMatchObjectMatchExpression*>(node);

        // If the array uses an index on its children, it's something like
        // {foo: {$elemMatch: {bar: 1}}}, in which case the predicate is really over foo.bar.
        prefix += node->path().toString() + ".";
        for (size_t i = 0; i < node->numChildren(); ++i) {
            _rateIndices(node->getChild(i), prefix, indices, collator, newContext);
        }
    } else if (node->getCategory() == MatchExpression::MatchCategory::kLogical) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            _rateIndices(node->getChild(i), prefix, indices, collator, elemMatchCtx);
        }
    }
}

// static
void QueryPlannerIXSelect::stripInvalidAssignments(MatchExpression* node,
                                                   const vector<IndexEntry>& indices) {
    stripInvalidAssignmentsToWildcardIndexes(node, indices);
    stripInvalidAssignmentsToTextIndexes(node, indices);

    if (MatchExpression::GEO != node->matchType() &&
        MatchExpression::GEO_NEAR != node->matchType()) {
        stripInvalidAssignmentsTo2dsphereIndices(node, indices);
    }

    stripInvalidAssignmentsToPartialIndices(node, indices);
}

namespace {

/**
 * For every node in the subtree rooted at 'node' that has a RelevantTag, removes index
 * assignments from that tag.
 *
 * Used as a helper for stripUnneededAssignments().
 */
void clearAssignments(MatchExpression* node) {
    if (node->getTag()) {
        RelevantTag* rt = static_cast<RelevantTag*>(node->getTag());
        rt->first.clear();
        rt->notFirst.clear();
    }

    for (size_t i = 0; i < node->numChildren(); i++) {
        clearAssignments(node->getChild(i));
    }
}

/**
 * Finds bounds-generating leaf nodes in the subtree rooted at 'node' that are logically AND'ed
 * together in the match expression tree, and returns them in the 'andRelated' out-parameter.
 * Logical nodes like OR and array nodes other than elemMatch object are instead returned in the
 * 'other' out-parameter.
 */
void partitionAndRelatedPreds(MatchExpression* node,
                              std::vector<MatchExpression*>* andRelated,
                              std::vector<MatchExpression*>* other) {
    for (size_t i = 0; i < node->numChildren(); ++i) {
        MatchExpression* child = node->getChild(i);
        if (Indexability::isBoundsGenerating(child)) {
            andRelated->push_back(child);
        } else if (MatchExpression::ELEM_MATCH_OBJECT == child->matchType() ||
                   MatchExpression::AND == child->matchType()) {
            partitionAndRelatedPreds(child, andRelated, other);
        } else {
            other->push_back(child);
        }
    }
}

}  // namespace

// static
void QueryPlannerIXSelect::stripUnneededAssignments(MatchExpression* node,
                                                    const std::vector<IndexEntry>& indices) {
    if (MatchExpression::AND == node->matchType()) {
        for (size_t i = 0; i < node->numChildren(); i++) {
            MatchExpression* child = node->getChild(i);

            if (MatchExpression::EQ != child->matchType()) {
                continue;
            }

            if (!child->getTag()) {
                continue;
            }

            // We found a EQ child of an AND which is tagged.
            RelevantTag* rt = static_cast<RelevantTag*>(child->getTag());

            // Look through all of the indices for which this predicate can be answered with
            // the leading field of the index.
            for (std::vector<size_t>::const_iterator i = rt->first.begin(); i != rt->first.end();
                 ++i) {
                size_t index = *i;

                if (indices[index].unique && 1 == indices[index].keyPattern.nFields()) {
                    // Found an EQ predicate which can use a single-field unique index.
                    // Clear assignments from the entire tree, and add back a single assignment
                    // for 'child' to the unique index.
                    clearAssignments(node);
                    RelevantTag* newRt = static_cast<RelevantTag*>(child->getTag());
                    newRt->first.push_back(index);

                    // Tag state has been reset in the entire subtree at 'root'; nothing
                    // else for us to do.
                    return;
                }
            }
        }
    }

    for (size_t i = 0; i < node->numChildren(); i++) {
        stripUnneededAssignments(node->getChild(i), indices);
    }
}

//
// Helpers used by stripInvalidAssignments
//

/**
 * Remove 'idx' from the RelevantTag lists for 'node'.  'node' must be a leaf.
 */
static void removeIndexRelevantTag(MatchExpression* node, size_t idx) {
    RelevantTag* tag = static_cast<RelevantTag*>(node->getTag());
    verify(tag);
    vector<size_t>::iterator firstIt = std::find(tag->first.begin(), tag->first.end(), idx);
    if (firstIt != tag->first.end()) {
        tag->first.erase(firstIt);
    }

    vector<size_t>::iterator notFirstIt =
        std::find(tag->notFirst.begin(), tag->notFirst.end(), idx);
    if (notFirstIt != tag->notFirst.end()) {
        tag->notFirst.erase(notFirstIt);
    }
}

namespace {

bool nodeIsNegationOrElemMatchObj(const MatchExpression* node) {
    return (node->matchType() == MatchExpression::NOT ||
            node->matchType() == MatchExpression::NOR ||
            node->matchType() == MatchExpression::ELEM_MATCH_OBJECT);
}

void stripInvalidAssignmentsToPartialIndexNode(MatchExpression* node,
                                               size_t idxNo,
                                               const IndexEntry& idxEntry,
                                               bool inNegationOrElemMatchObj) {
    if (node->getTag()) {
        removeIndexRelevantTag(node, idxNo);
    }
    inNegationOrElemMatchObj |= nodeIsNegationOrElemMatchObj(node);
    for (size_t i = 0; i < node->numChildren(); ++i) {
        // If 'node' is an OR and our current clause satisfies the filter expression, then we may be
        // able to spare this clause from being stripped.  We only support such sparing if we're not
        // in a negation or ELEM_MATCH_OBJECT, though:
        // - If we're in a negation, then we're looking for documents that describe the inverse of
        //   the current predicate.  For example, suppose we have an index with key pattern {a: 1}
        //   and filter expression {a: {$gt: 0}}, and our OR is inside a negation and the current
        //   clause we're evaluating is {a: 10}.  If we allow use of this index, then we'd end up
        //   generating bounds corresponding to the predicate {a: {$ne: 10}}, which does not satisfy
        //   the filter expression (note also that the match expression parser currently does not
        //   generate trees with OR inside NOT, but we may consider changing it to allow this in the
        //   future).
        // - If we're in an ELEM_MATCH_OBJECT, then every predicate in the current clause has an
        //   implicit prefix of the elemMatch's path, so it can't be compared outright to the filter
        //   expression.  For example, suppose we have an index with key pattern {"a.b": 1} and
        //   filter expression {f: 1}, and we are evaluating the first clause of the $or in the
        //   query {a: {$elemMatch: {$or: [{b: 1, f: 1}, ...]}}}.  Even though {b: 1, f: 1}
        //   satisfies the filter expression {f: 1}, the former is referring to fields "a.b" and
        //   "a.f" while the latter is referring to field "f", so the clause does not actually
        //   satisfy the filter expression and should not be spared.
        if (!inNegationOrElemMatchObj && node->matchType() == MatchExpression::OR &&
            expression::isSubsetOf(node->getChild(i), idxEntry.filterExpr)) {
            continue;
        }
        stripInvalidAssignmentsToPartialIndexNode(
            node->getChild(i), idxNo, idxEntry, inNegationOrElemMatchObj);
    }
}

void stripInvalidAssignmentsToPartialIndexRoot(MatchExpression* root,
                                               size_t idxNo,
                                               const IndexEntry& idxEntry) {
    if (expression::isSubsetOf(root, idxEntry.filterExpr)) {
        return;
    }
    const bool inNegationOrElemMatchObj = nodeIsNegationOrElemMatchObj(root);
    stripInvalidAssignmentsToPartialIndexNode(root, idxNo, idxEntry, inNegationOrElemMatchObj);
}

}  // namespace

void QueryPlannerIXSelect::stripInvalidAssignmentsToPartialIndices(
    MatchExpression* node, const vector<IndexEntry>& indices) {
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i].filterExpr) {
            stripInvalidAssignmentsToPartialIndexRoot(node, i, indices[i]);
        }
    }
}

//
// Wildcard index invalid assignments.
//
void QueryPlannerIXSelect::stripInvalidAssignmentsToWildcardIndexes(
    MatchExpression* root, const vector<IndexEntry>& indices) {
    for (size_t idx = 0; idx < indices.size(); ++idx) {
        // Skip over all indexes except $**.
        if (indices[idx].type != IndexType::INDEX_WILDCARD) {
            continue;
        }
        // If we have a $** index, check whether we have a TEXT node in the MatchExpression tree.
        const std::function<MatchExpression*(MatchExpression*)> findTextNode = [&](auto* node) {
            if (node->matchType() == MatchExpression::TEXT) {
                return node;
            }
            for (size_t i = 0; i < node->numChildren(); ++i) {
                if (auto* foundNode = findTextNode(node->getChild(i)))
                    return foundNode;
            }
            return static_cast<MatchExpression*>(nullptr);
        };
        // If so, remove the $** index from the node's relevant tags.
        if (auto* textNode = findTextNode(root)) {
            removeIndexRelevantTag(textNode, idx);
        }
    }
}

//
// Text index quirks
//

/**
 * Traverse the subtree rooted at 'node' to remove invalid RelevantTag assignments to text index
 * 'idx', which has prefix paths 'prefixPaths'.
 */
static void stripInvalidAssignmentsToTextIndex(MatchExpression* node,
                                               size_t idx,
                                               const StringDataUnorderedSet& prefixPaths) {
    // If we're here, there are prefixPaths and node is either:
    // 1. a text pred which we can't use as we have nothing over its prefix, or
    // 2. a non-text pred which we can't use as we don't have a text pred AND-related.
    if (Indexability::nodeCanUseIndexOnOwnField(node)) {
        removeIndexRelevantTag(node, idx);
        return;
    }

    // Do not traverse tree beyond negation node.
    if (node->matchType() == MatchExpression::NOT || node->matchType() == MatchExpression::NOR) {
        return;
    }

    // For anything to use a text index with prefixes, we require that:
    // 1. The text pred exists in an AND,
    // 2. The non-text preds that use the text index's prefixes are also in that AND.

    if (node->matchType() != MatchExpression::AND) {
        // It's an OR or some kind of array operator.
        for (size_t i = 0; i < node->numChildren(); ++i) {
            stripInvalidAssignmentsToTextIndex(node->getChild(i), idx, prefixPaths);
        }
        return;
    }

    // If we're here, we're an AND.  Determine whether the children satisfy the index prefix for
    // the text index.
    invariant(node->matchType() == MatchExpression::AND);

    bool hasText = false;

    // The AND must have an EQ predicate for each prefix path.  When we encounter a child with a
    // tag we remove it from childrenPrefixPaths.  All children exist if this set is empty at
    // the end.
    StringDataUnorderedSet childrenPrefixPaths = prefixPaths;

    for (size_t i = 0; i < node->numChildren(); ++i) {
        MatchExpression* child = node->getChild(i);
        RelevantTag* tag = static_cast<RelevantTag*>(child->getTag());

        if (nullptr == tag) {
            // 'child' could be a logical operator.  Maybe there are some assignments hiding
            // inside.
            stripInvalidAssignmentsToTextIndex(child, idx, prefixPaths);
            continue;
        }

        bool inFirst = tag->first.end() != std::find(tag->first.begin(), tag->first.end(), idx);

        bool inNotFirst =
            tag->notFirst.end() != std::find(tag->notFirst.begin(), tag->notFirst.end(), idx);

        if (inFirst || inNotFirst) {
            // Great!  'child' was assigned to our index.
            if (child->matchType() == MatchExpression::TEXT) {
                hasText = true;
            } else {
                childrenPrefixPaths.erase(child->path());
                // One fewer prefix we're looking for, possibly.  Note that we could have a
                // suffix assignment on the index and wind up here.  In this case the erase
                // above won't do anything since a suffix isn't a prefix.
            }
        } else {
            // Recurse on the children to ensure that they're not hiding any assignments
            // to idx.
            stripInvalidAssignmentsToTextIndex(child, idx, prefixPaths);
        }
    }

    // Our prereqs for using the text index were not satisfied so we remove the assignments from
    // all children of the AND.
    if (!hasText || !childrenPrefixPaths.empty()) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            stripInvalidAssignmentsToTextIndex(node->getChild(i), idx, prefixPaths);
        }
    }
}

// static
void QueryPlannerIXSelect::stripInvalidAssignmentsToTextIndexes(MatchExpression* node,
                                                                const vector<IndexEntry>& indices) {
    for (size_t i = 0; i < indices.size(); ++i) {
        const IndexEntry& index = indices[i];

        // We only care about text indices.
        if (INDEX_TEXT != index.type) {
            continue;
        }

        // Gather the set of paths that comprise the index prefix for this text index.
        // Each of those paths must have an equality assignment, otherwise we can't assign
        // *anything* to this index.
        auto textIndexPrefixPaths =
            SimpleStringDataComparator::kInstance.makeStringDataUnorderedSet();
        BSONObjIterator it(index.keyPattern);

        // We stop when we see the first string in the key pattern.  We know that
        // the prefix precedes "text".
        for (BSONElement elt = it.next(); elt.type() != String; elt = it.next()) {
            textIndexPrefixPaths.insert(elt.fieldName());
            verify(it.more());
        }

        // If the index prefix is non-empty, remove invalid assignments to it.
        if (!textIndexPrefixPaths.empty()) {
            stripInvalidAssignmentsToTextIndex(node, i, textIndexPrefixPaths);
        }
    }
}

//
// 2dsphere V2 sparse quirks
//

static void stripInvalidAssignmentsTo2dsphereIndex(MatchExpression* node, size_t idx) {
    if (Indexability::nodeCanUseIndexOnOwnField(node) &&
        MatchExpression::GEO != node->matchType() &&
        MatchExpression::GEO_NEAR != node->matchType()) {
        // We found a non-geo predicate tagged to use a V2 2dsphere index which is not
        // and-related to a geo predicate that can use the index.
        removeIndexRelevantTag(node, idx);
        return;
    }

    const MatchExpression::MatchType nodeType = node->matchType();

    // Don't bother peeking inside of negations.
    if (MatchExpression::NOT == nodeType || MatchExpression::NOR == nodeType) {
        return;
    }

    if (MatchExpression::AND != nodeType) {
        // It's an OR or some kind of array operator.
        for (size_t i = 0; i < node->numChildren(); ++i) {
            stripInvalidAssignmentsTo2dsphereIndex(node->getChild(i), idx);
        }
        return;
    }

    bool hasGeoField = false;

    // Split 'node' into those leaf predicates that are logically AND-related and everything else.
    std::vector<MatchExpression*> andRelated;
    std::vector<MatchExpression*> other;
    partitionAndRelatedPreds(node, &andRelated, &other);

    // Traverse through non and-related leaf nodes. These are generally logical nodes like OR, and
    // there may be some assignments hiding inside that need to be stripped.
    for (auto child : other) {
        stripInvalidAssignmentsTo2dsphereIndex(child, idx);
    }

    // Traverse through the and-related leaf nodes. We strip all assignments to such nodes unless we
    // find an assigned geo predicate.
    for (auto child : andRelated) {
        RelevantTag* tag = static_cast<RelevantTag*>(child->getTag());

        if (!tag) {
            // No tags to strip.
            continue;
        }

        bool inFirst = tag->first.end() != std::find(tag->first.begin(), tag->first.end(), idx);

        bool inNotFirst =
            tag->notFirst.end() != std::find(tag->notFirst.begin(), tag->notFirst.end(), idx);

        // If there is an index assignment...
        if (inFirst || inNotFirst) {
            // And it's a geo predicate...
            if (MatchExpression::GEO == child->matchType() ||
                MatchExpression::GEO_NEAR == child->matchType()) {
                hasGeoField = true;
            }
        }
    }

    // If there isn't a geo predicate our results aren't a subset of what's in the geo index, so
    // if we use the index we'll miss results.
    if (!hasGeoField) {
        for (auto child : andRelated) {
            stripInvalidAssignmentsTo2dsphereIndex(child, idx);
        }
    }
}

// static
void QueryPlannerIXSelect::stripInvalidAssignmentsTo2dsphereIndices(
    MatchExpression* node, const vector<IndexEntry>& indices) {
    for (size_t i = 0; i < indices.size(); ++i) {
        const IndexEntry& index = indices[i];

        // We only worry about 2dsphere indices.
        if (INDEX_2DSPHERE != index.type) {
            continue;
        }

        // 2dsphere version 1 indices do not have the geo-sparseness property, so there's no need to
        // strip assignments to such indices.
        BSONElement elt = index.infoObj["2dsphereIndexVersion"];
        if (elt.eoo()) {
            continue;
        }
        if (!elt.isNumber()) {
            continue;
        }
        if (S2_INDEX_VERSION_1 == elt.numberInt()) {
            continue;
        }

        // If every field is geo don't bother doing anything.
        bool allFieldsGeo = true;
        BSONObjIterator it(index.keyPattern);
        while (it.more()) {
            BSONElement elt = it.next();
            if (String != elt.type()) {
                allFieldsGeo = false;
                break;
            }
        }
        if (allFieldsGeo) {
            continue;
        }

        // Remove bad assignments from this index.
        stripInvalidAssignmentsTo2dsphereIndex(node, i);
    }
}

}  // namespace mongo
