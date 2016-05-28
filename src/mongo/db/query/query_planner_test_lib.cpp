/**
 *    Copyright (C) 2013 10gen Inc.
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

/**
 * This file contains tests for mongo/db/query/query_planner.cpp
 */

#include "mongo/db/query/query_planner_test_lib.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include <ostream>

namespace {

using namespace mongo;

using std::string;

bool filterMatches(const BSONObj& testFilter,
                   const BSONObj& testCollation,
                   const QuerySolutionNode* trueFilterNode) {
    if (NULL == trueFilterNode->filter) {
        return false;
    }

    std::unique_ptr<CollatorInterface> testCollator;
    if (!testCollation.isEmpty()) {
        CollatorFactoryMock collatorFactoryMock;
        auto collator = collatorFactoryMock.makeFromBSON(testCollation);
        if (!collator.isOK()) {
            return false;
        }
        testCollator = std::move(collator.getValue());
    }

    StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
        testFilter, ExtensionsCallbackDisallowExtensions(), testCollator.get());
    if (!statusWithMatcher.isOK()) {
        return false;
    }
    const std::unique_ptr<MatchExpression> root = std::move(statusWithMatcher.getValue());
    CanonicalQuery::sortTree(root.get());
    std::unique_ptr<MatchExpression> trueFilter(trueFilterNode->filter->shallowClone());
    CanonicalQuery::sortTree(trueFilter.get());
    return trueFilter->equivalent(root.get());
}

void appendIntervalBound(BSONObjBuilder& bob, BSONElement& el) {
    if (el.type() == String) {
        std::string data = el.String();
        if (data == "MaxKey") {
            bob.appendMaxKey("");
        } else if (data == "MinKey") {
            bob.appendMinKey("");
        } else {
            bob.appendAs(el, "");
        }
    } else {
        bob.appendAs(el, "");
    }
}

bool intervalMatches(const BSONObj& testInt, const Interval trueInt) {
    BSONObjIterator it(testInt);
    if (!it.more()) {
        return false;
    }
    BSONElement low = it.next();
    if (!it.more()) {
        return false;
    }
    BSONElement high = it.next();
    if (!it.more()) {
        return false;
    }
    bool startInclusive = it.next().Bool();
    if (!it.more()) {
        return false;
    }
    bool endInclusive = it.next().Bool();
    if (it.more()) {
        return false;
    }

    BSONObjBuilder bob;
    appendIntervalBound(bob, low);
    appendIntervalBound(bob, high);
    Interval toCompare(bob.obj(), startInclusive, endInclusive);

    return Interval::INTERVAL_EQUALS == trueInt.compare(toCompare);
}

/**
 * Returns whether the BSON representation of the index bounds in
 * 'testBounds' matches 'trueBounds'.
 *
 * 'testBounds' should be of the following format:
 *    {<field 1>: <oil 1>, <field 2>: <oil 2>, ...}
 * Each ordered interval list (e.g. <oil 1>) is an array of arrays of
 * the format:
 *    [[<low 1>,<high 1>,<lowInclusive 1>,<highInclusive 1>], ...]
 *
 * For example,
 *    {a: [[1,2,true,false], [3,4,false,true]], b: [[-Infinity, Infinity]]}
 * Means that the index bounds on field 'a' consist of the two intervals
 * [1, 2) and (3, 4] and the index bounds on field 'b' are [-Infinity, Infinity].
 */
bool boundsMatch(const BSONObj& testBounds, const IndexBounds trueBounds) {
    // Iterate over the fields on which we have index bounds.
    BSONObjIterator fieldIt(testBounds);
    int fieldItCount = 0;
    while (fieldIt.more()) {
        BSONElement arrEl = fieldIt.next();
        if (arrEl.fieldNameStringData() != trueBounds.getFieldName(fieldItCount)) {
            return false;
        }
        if (arrEl.type() != Array) {
            return false;
        }
        // Iterate over an ordered interval list for
        // a particular field.
        BSONObjIterator oilIt(arrEl.Obj());
        int oilItCount = 0;
        while (oilIt.more()) {
            BSONElement intervalEl = oilIt.next();
            if (intervalEl.type() != Array) {
                return false;
            }
            Interval trueInt = trueBounds.getInterval(fieldItCount, oilItCount);
            if (!intervalMatches(intervalEl.Obj(), trueInt)) {
                return false;
            }
            ++oilItCount;
        }
        ++fieldItCount;
    }

    return true;
}

}  // namespace

namespace mongo {

/**
 * Looks in the children stored in the 'nodes' field of 'testSoln'
 * to see if thet match the 'children' field of 'trueSoln'.
 *
 * This does an unordered comparison, i.e. childrenMatch returns
 * true as long as the set of subtrees in testSoln's 'nodes' matches
 * the set of subtrees in trueSoln's 'children' vector.
 */
static bool childrenMatch(const BSONObj& testSoln, const QuerySolutionNode* trueSoln) {
    BSONElement children = testSoln["nodes"];
    if (children.eoo() || !children.isABSONObj()) {
        return false;
    }

    // The order of the children array in testSoln might not match
    // the order in trueSoln, so we have to check all combos with
    // these nested loops.
    BSONObjIterator i(children.Obj());
    while (i.more()) {
        BSONElement child = i.next();
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        // try to match against one of the QuerySolutionNode's children
        bool found = false;
        for (size_t j = 0; j < trueSoln->children.size(); ++j) {
            if (QueryPlannerTestLib::solutionMatches(child.Obj(), trueSoln->children[j])) {
                found = true;
                break;
            }
        }

        // we couldn't match child
        if (!found) {
            return false;
        }
    }

    return true;
}

// static
bool QueryPlannerTestLib::solutionMatches(const BSONObj& testSoln,
                                          const QuerySolutionNode* trueSoln) {
    //
    // leaf nodes
    //
    if (STAGE_COLLSCAN == trueSoln->getType()) {
        const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(trueSoln);
        BSONElement el = testSoln["cscan"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj csObj = el.Obj();

        BSONElement dir = csObj["dir"];
        if (dir.eoo() || !dir.isNumber()) {
            return false;
        }
        if (dir.numberInt() != csn->direction) {
            return false;
        }

        BSONElement filter = csObj["filter"];
        if (filter.eoo()) {
            return true;
        } else if (filter.isNull()) {
            return NULL == csn->filter;
        } else if (!filter.isABSONObj()) {
            return false;
        }

        BSONObj collation;
        if (BSONElement collationElt = csObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        return filterMatches(filter.Obj(), collation, trueSoln);
    } else if (STAGE_IXSCAN == trueSoln->getType()) {
        const IndexScanNode* ixn = static_cast<const IndexScanNode*>(trueSoln);
        BSONElement el = testSoln["ixscan"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj ixscanObj = el.Obj();

        BSONElement pattern = ixscanObj["pattern"];
        if (pattern.eoo() || !pattern.isABSONObj()) {
            return false;
        }
        if (pattern.Obj() != ixn->indexKeyPattern) {
            return false;
        }

        BSONElement bounds = ixscanObj["bounds"];
        if (!bounds.eoo()) {
            if (!bounds.isABSONObj()) {
                return false;
            } else if (!boundsMatch(bounds.Obj(), ixn->bounds)) {
                return false;
            }
        }

        BSONElement dir = ixscanObj["dir"];
        if (!dir.eoo() && NumberInt == dir.type()) {
            if (dir.numberInt() != ixn->direction) {
                return false;
            }
        }

        BSONElement filter = ixscanObj["filter"];
        if (filter.eoo()) {
            return true;
        } else if (filter.isNull()) {
            return NULL == ixn->filter;
        } else if (!filter.isABSONObj()) {
            return false;
        }

        BSONObj collation;
        if (BSONElement collationElt = ixscanObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        return filterMatches(filter.Obj(), collation, trueSoln);
    } else if (STAGE_GEO_NEAR_2D == trueSoln->getType()) {
        const GeoNear2DNode* node = static_cast<const GeoNear2DNode*>(trueSoln);
        BSONElement el = testSoln["geoNear2d"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj geoObj = el.Obj();
        return geoObj == node->indexKeyPattern;
    } else if (STAGE_GEO_NEAR_2DSPHERE == trueSoln->getType()) {
        const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(trueSoln);
        BSONElement el = testSoln["geoNear2dsphere"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj geoObj = el.Obj();

        BSONElement pattern = geoObj["pattern"];
        if (pattern.eoo() || !pattern.isABSONObj()) {
            return false;
        }
        if (pattern.Obj() != node->indexKeyPattern) {
            return false;
        }

        BSONElement bounds = geoObj["bounds"];
        if (!bounds.eoo()) {
            if (!bounds.isABSONObj()) {
                return false;
            } else if (!boundsMatch(bounds.Obj(), node->baseBounds)) {
                return false;
            }
        }

        return true;
    } else if (STAGE_TEXT == trueSoln->getType()) {
        // {text: {search: "somestr", language: "something", filter: {blah: 1}}}
        const TextNode* node = static_cast<const TextNode*>(trueSoln);
        BSONElement el = testSoln["text"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj textObj = el.Obj();

        BSONElement searchElt = textObj["search"];
        if (!searchElt.eoo()) {
            if (searchElt.String() != node->ftsQuery->getQuery()) {
                return false;
            }
        }

        BSONElement languageElt = textObj["language"];
        if (!languageElt.eoo()) {
            if (languageElt.String() != node->ftsQuery->getLanguage()) {
                return false;
            }
        }

        BSONElement caseSensitiveElt = textObj["caseSensitive"];
        if (!caseSensitiveElt.eoo()) {
            if (caseSensitiveElt.trueValue() != node->ftsQuery->getCaseSensitive()) {
                return false;
            }
        }

        BSONElement diacriticSensitiveElt = textObj["diacriticSensitive"];
        if (!diacriticSensitiveElt.eoo()) {
            if (diacriticSensitiveElt.trueValue() != node->ftsQuery->getDiacriticSensitive()) {
                return false;
            }
        }

        BSONElement indexPrefix = textObj["prefix"];
        if (!indexPrefix.eoo()) {
            if (!indexPrefix.isABSONObj()) {
                return false;
            }

            if (0 != indexPrefix.Obj().woCompare(node->indexPrefix)) {
                return false;
            }
        }

        BSONObj collation;
        if (BSONElement collationElt = textObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = textObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (NULL != node->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        return true;
    }

    //
    // internal nodes
    //
    if (STAGE_FETCH == trueSoln->getType()) {
        const FetchNode* fn = static_cast<const FetchNode*>(trueSoln);

        BSONElement el = testSoln["fetch"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj fetchObj = el.Obj();

        BSONObj collation;
        if (BSONElement collationElt = fetchObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = fetchObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (NULL != fn->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        BSONElement child = fetchObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }
        return solutionMatches(child.Obj(), fn->children[0]);
    } else if (STAGE_OR == trueSoln->getType()) {
        const OrNode* orn = static_cast<const OrNode*>(trueSoln);
        BSONElement el = testSoln["or"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj orObj = el.Obj();
        return childrenMatch(orObj, orn);
    } else if (STAGE_AND_HASH == trueSoln->getType()) {
        const AndHashNode* ahn = static_cast<const AndHashNode*>(trueSoln);
        BSONElement el = testSoln["andHash"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj andHashObj = el.Obj();

        BSONObj collation;
        if (BSONElement collationElt = andHashObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = andHashObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (NULL != ahn->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        return childrenMatch(andHashObj, ahn);
    } else if (STAGE_AND_SORTED == trueSoln->getType()) {
        const AndSortedNode* asn = static_cast<const AndSortedNode*>(trueSoln);
        BSONElement el = testSoln["andSorted"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj andSortedObj = el.Obj();

        BSONObj collation;
        if (BSONElement collationElt = andSortedObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = andSortedObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (NULL != asn->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        return childrenMatch(andSortedObj, asn);
    } else if (STAGE_PROJECTION == trueSoln->getType()) {
        const ProjectionNode* pn = static_cast<const ProjectionNode*>(trueSoln);

        BSONElement el = testSoln["proj"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj projObj = el.Obj();

        BSONElement projType = projObj["type"];
        if (!projType.eoo()) {
            string projTypeStr = projType.str();
            if (!((pn->projType == ProjectionNode::DEFAULT && projTypeStr == "default") ||
                  (pn->projType == ProjectionNode::SIMPLE_DOC && projTypeStr == "simple") ||
                  (pn->projType == ProjectionNode::COVERED_ONE_INDEX &&
                   projTypeStr == "coveredIndex"))) {
                return false;
            }
        }

        BSONElement spec = projObj["spec"];
        if (spec.eoo() || !spec.isABSONObj()) {
            return false;
        }
        BSONElement child = projObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return (spec.Obj() == pn->projection) && solutionMatches(child.Obj(), pn->children[0]);
    } else if (STAGE_SORT == trueSoln->getType()) {
        const SortNode* sn = static_cast<const SortNode*>(trueSoln);
        BSONElement el = testSoln["sort"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj sortObj = el.Obj();

        BSONElement patternEl = sortObj["pattern"];
        if (patternEl.eoo() || !patternEl.isABSONObj()) {
            return false;
        }
        BSONElement limitEl = sortObj["limit"];
        if (!limitEl.isNumber()) {
            return false;
        }
        BSONElement child = sortObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        size_t expectedLimit = limitEl.numberInt();
        return (patternEl.Obj() == sn->pattern) && (expectedLimit == sn->limit) &&
            solutionMatches(child.Obj(), sn->children[0]);
    } else if (STAGE_SORT_KEY_GENERATOR == trueSoln->getType()) {
        const SortKeyGeneratorNode* keyGenNode = static_cast<const SortKeyGeneratorNode*>(trueSoln);
        BSONElement el = testSoln["sortKeyGen"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj keyGenObj = el.Obj();

        BSONElement child = keyGenObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return solutionMatches(child.Obj(), keyGenNode->children[0]);
    } else if (STAGE_SORT_MERGE == trueSoln->getType()) {
        const MergeSortNode* msn = static_cast<const MergeSortNode*>(trueSoln);
        BSONElement el = testSoln["mergeSort"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj mergeSortObj = el.Obj();
        return childrenMatch(mergeSortObj, msn);
    } else if (STAGE_SKIP == trueSoln->getType()) {
        const SkipNode* sn = static_cast<const SkipNode*>(trueSoln);
        BSONElement el = testSoln["skip"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj sortObj = el.Obj();

        BSONElement skipEl = sortObj["n"];
        if (!skipEl.isNumber()) {
            return false;
        }
        BSONElement child = sortObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return (skipEl.numberInt() == sn->skip) && solutionMatches(child.Obj(), sn->children[0]);
    } else if (STAGE_LIMIT == trueSoln->getType()) {
        const LimitNode* ln = static_cast<const LimitNode*>(trueSoln);
        BSONElement el = testSoln["limit"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj sortObj = el.Obj();

        BSONElement limitEl = sortObj["n"];
        if (!limitEl.isNumber()) {
            return false;
        }
        BSONElement child = sortObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return (limitEl.numberInt() == ln->limit) && solutionMatches(child.Obj(), ln->children[0]);
    } else if (STAGE_KEEP_MUTATIONS == trueSoln->getType()) {
        const KeepMutationsNode* kn = static_cast<const KeepMutationsNode*>(trueSoln);

        BSONElement el = testSoln["keep"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj keepObj = el.Obj();

        // Doesn't have any parameters really.
        BSONElement child = keepObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return solutionMatches(child.Obj(), kn->children[0]);
    } else if (STAGE_SHARDING_FILTER == trueSoln->getType()) {
        const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(trueSoln);

        BSONElement el = testSoln["sharding_filter"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj keepObj = el.Obj();

        BSONElement child = keepObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return solutionMatches(child.Obj(), fn->children[0]);
    } else if (STAGE_ENSURE_SORTED == trueSoln->getType()) {
        const EnsureSortedNode* esn = static_cast<const EnsureSortedNode*>(trueSoln);

        BSONElement el = testSoln["ensureSorted"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj esObj = el.Obj();

        BSONElement patternEl = esObj["pattern"];
        if (patternEl.eoo() || !patternEl.isABSONObj()) {
            return false;
        }
        BSONElement child = esObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return (patternEl.Obj() == esn->pattern) && solutionMatches(child.Obj(), esn->children[0]);
    }

    return false;
}

}  // namespace mongo
