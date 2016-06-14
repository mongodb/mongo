/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/query/planner_ixselect.h"

#include <vector>

#include "mongo/db/geo/hash.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/log.h"

namespace mongo {

static double fieldWithDefault(const BSONObj& infoObj, const string& name, double def) {
    BSONElement e = infoObj[name];
    if (e.isNumber()) {
        return e.numberDouble();
    }
    return def;
}

/**
 * 2d indices don't handle wrapping so we can't use them for queries that wrap.
 */
static bool twoDWontWrap(const Circle& circle, const IndexEntry& index) {
    GeoHashConverter::Parameters hashParams;
    Status paramStatus = GeoHashConverter::parseParameters(index.infoObj, &hashParams);
    verify(paramStatus.isOK());  // we validated the params on index creation

    GeoHashConverter conv(hashParams);

    // FYI: old code used flat not spherical error.
    double yscandist = rad2deg(circle.radius) + conv.getErrorSphere();
    double xscandist = computeXScanDistance(circle.center.y, yscandist);
    bool ret = circle.center.x + xscandist < 180 && circle.center.x - xscandist > -180 &&
        circle.center.y + yscandist < 90 && circle.center.y - yscandist > -90;
    return ret;
}

// Checks whether 'node' contains any comparison to an element of type 'type'. Nested objects and
// arrays are not checked recursively. We assume 'node' is bounds-generating or is a recursive child
// of a bounds-generating node, i.e. it does not contain AND, OR, ELEM_MATCH_OBJECT, or NOR.
// TODO SERVER-23172: Check nested objects and arrays.
static bool boundsGeneratingNodeContainsComparisonToType(MatchExpression* node, BSONType type) {
    invariant(node->matchType() != MatchExpression::AND &&
              node->matchType() != MatchExpression::OR &&
              node->matchType() != MatchExpression::NOR &&
              node->matchType() != MatchExpression::ELEM_MATCH_OBJECT);

    if (Indexability::isEqualityOrInequality(node)) {
        const ComparisonMatchExpression* expr = static_cast<const ComparisonMatchExpression*>(node);
        return expr->getData().type() == type;
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
                                     unordered_set<string>* out) {
    // Do not traverse tree beyond a NOR negation node
    MatchExpression::MatchType exprtype = node->matchType();
    if (exprtype == MatchExpression::NOR) {
        return;
    }

    // Leaf nodes with a path and some array operators.
    if (Indexability::nodeCanUseIndexOnOwnField(node)) {
        out->insert(prefix + node->path().toString());
    } else if (Indexability::arrayUsesIndexOnChildren(node)) {
        // If the array uses an index on its children, it's something like
        // {foo : {$elemMatch: { bar: 1}}}, in which case the predicate is really over
        // foo.bar.
        //
        // When we have {foo: {$all: [{$elemMatch: {a:1}}], the path of the embedded elemMatch
        // is empty.  We don't want to append a dot in that case as the field would be foo..a.
        if (!node->path().empty()) {
            prefix += node->path().toString() + ".";
        }

        for (size_t i = 0; i < node->numChildren(); ++i) {
            getFields(node->getChild(i), prefix, out);
        }
    } else if (node->isLogical()) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            getFields(node->getChild(i), prefix, out);
        }
    }
}

// static
void QueryPlannerIXSelect::findRelevantIndices(const unordered_set<string>& fields,
                                               const vector<IndexEntry>& allIndices,
                                               vector<IndexEntry>* out) {
    for (size_t i = 0; i < allIndices.size(); ++i) {
        BSONObjIterator it(allIndices[i].keyPattern);
        verify(it.more());
        BSONElement elt = it.next();
        if (fields.end() != fields.find(elt.fieldName())) {
            out->push_back(allIndices[i]);
        }
    }
}

// static
bool QueryPlannerIXSelect::compatible(const BSONElement& elt,
                                      const IndexEntry& index,
                                      MatchExpression* node,
                                      const CollatorInterface* collator) {
    if ((boundsGeneratingNodeContainsComparisonToType(node, BSONType::String) ||
         boundsGeneratingNodeContainsComparisonToType(node, BSONType::Array) ||
         boundsGeneratingNodeContainsComparisonToType(node, BSONType::Object)) &&
        !CollatorInterface::collatorsMatch(collator, index.collator)) {
        return false;
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
    if (String != elt.type() || (INDEX_BTREE == index.type)) {
        indexedFieldType = "";
    } else {
        indexedFieldType = elt.String();
    }

    // We know elt.fieldname() == node->path().
    MatchExpression::MatchType exprtype = node->matchType();

    if (indexedFieldType.empty()) {
        // Can't check for null w/a sparse index.
        if (exprtype == MatchExpression::EQ && index.sparse) {
            const EqualityMatchExpression* expr = static_cast<const EqualityMatchExpression*>(node);
            if (expr->getData().isNull()) {
                return false;
            }
        }

        // Can't check for $in w/ null element w/a sparse index.
        if (exprtype == MatchExpression::MATCH_IN && index.sparse) {
            const InMatchExpression* expr = static_cast<const InMatchExpression*>(node);
            if (expr->hasNull()) {
                return false;
            }
        }

        // We can't use a btree-indexed field for geo expressions.
        if (exprtype == MatchExpression::GEO || exprtype == MatchExpression::GEO_NEAR) {
            return false;
        }

        // There are restrictions on when we can use the index if
        // the expression is a NOT.
        if (exprtype == MatchExpression::NOT) {
            // Don't allow indexed NOT on special index types such as geo or text indices.
            if (INDEX_BTREE != index.type) {
                return false;
            }

            // Prevent negated preds from using sparse indices. Doing so would cause us to
            // miss documents which do not contain the indexed fields.
            if (index.sparse) {
                return false;
            }

            // Can't index negations of MOD, REGEX, TYPE_OPERATOR, or ELEM_MATCH_VALUE.
            MatchExpression::MatchType childtype = node->getChild(0)->matchType();
            if (MatchExpression::REGEX == childtype || MatchExpression::MOD == childtype ||
                MatchExpression::TYPE_OPERATOR == childtype ||
                MatchExpression::ELEM_MATCH_VALUE == childtype) {
                return false;
            }

            // If it's a negated $in, it can't have any REGEX's inside.
            if (MatchExpression::MATCH_IN == childtype) {
                InMatchExpression* ime = static_cast<InMatchExpression*>(node->getChild(0));
                if (!ime->getRegexes().empty()) {
                    return false;
                }
            }
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
        BSONObjIterator specIt(index.keyPattern);
        while (specIt.more()) {
            BSONElement elt = specIt.next();
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

        // NOTE: This shouldn't be reached.  Text index implies there is a separator implies we
        // will always hit the 'return true' above.
        invariant(0);
        return true;
    } else if (IndexNames::HASHED == indexedFieldType) {
        if (exprtype == MatchExpression::EQ) {
            return true;
        }
        if (exprtype == MatchExpression::MATCH_IN) {
            const InMatchExpression* expr = static_cast<const InMatchExpression*>(node);
            return expr->getRegexes().empty();
        }
        return false;
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
            if (NULL == cap) {
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
        warning() << "Unknown indexing for node " << node->toString() << " and field "
                  << elt.toString() << endl;
        verify(0);
    }
}

// static
void QueryPlannerIXSelect::rateIndices(MatchExpression* node,
                                       string prefix,
                                       const vector<IndexEntry>& indices,
                                       const CollatorInterface* collator) {
    // Do not traverse tree beyond logical NOR node
    MatchExpression::MatchType exprtype = node->matchType();
    if (exprtype == MatchExpression::NOR) {
        return;
    }

    // Every indexable node is tagged even when no compatible index is
    // available.
    if (Indexability::isBoundsGenerating(node)) {
        string fullPath;
        if (MatchExpression::NOT == node->matchType()) {
            fullPath = prefix + node->getChild(0)->path().toString();
        } else {
            fullPath = prefix + node->path().toString();
        }

        verify(NULL == node->getTag());
        RelevantTag* rt = new RelevantTag();
        node->setTag(rt);
        rt->path = fullPath;

        // TODO: This is slow, with all the string compares.
        for (size_t i = 0; i < indices.size(); ++i) {
            BSONObjIterator it(indices[i].keyPattern);
            BSONElement elt = it.next();
            if (elt.fieldName() == fullPath && compatible(elt, indices[i], node, collator)) {
                rt->first.push_back(i);
            }
            while (it.more()) {
                elt = it.next();
                if (elt.fieldName() == fullPath && compatible(elt, indices[i], node, collator)) {
                    rt->notFirst.push_back(i);
                }
            }
        }

        // If this is a NOT, we have to clone the tag and attach
        // it to the NOT's child.
        if (MatchExpression::NOT == node->matchType()) {
            RelevantTag* childRt = static_cast<RelevantTag*>(rt->clone());
            childRt->path = rt->path;
            node->getChild(0)->setTag(childRt);
        }
    } else if (Indexability::arrayUsesIndexOnChildren(node)) {
        // See comment in getFields about all/elemMatch and paths.
        if (!node->path().empty()) {
            prefix += node->path().toString() + ".";
        }
        for (size_t i = 0; i < node->numChildren(); ++i) {
            rateIndices(node->getChild(i), prefix, indices, collator);
        }
    } else if (node->isLogical()) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            rateIndices(node->getChild(i), prefix, indices, collator);
        }
    }
}

// static
void QueryPlannerIXSelect::stripInvalidAssignments(MatchExpression* node,
                                                   const vector<IndexEntry>& indices) {
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
// Text index quirks
//

/**
 * Traverse the subtree rooted at 'node' to remove invalid RelevantTag assignments to text index
 * 'idx', which has prefix paths 'prefixPaths'.
 */
static void stripInvalidAssignmentsToTextIndex(
    MatchExpression* node,
    size_t idx,
    const unordered_set<StringData, StringData::Hasher>& prefixPaths) {
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
    unordered_set<StringData, StringData::Hasher> childrenPrefixPaths = prefixPaths;

    for (size_t i = 0; i < node->numChildren(); ++i) {
        MatchExpression* child = node->getChild(i);
        RelevantTag* tag = static_cast<RelevantTag*>(child->getTag());

        if (NULL == tag) {
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
        unordered_set<StringData, StringData::Hasher> textIndexPrefixPaths;
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
