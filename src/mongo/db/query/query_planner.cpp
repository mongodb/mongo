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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_planner.h"

#include <vector>

#include "mongo/client/dbclientinterface.h"  // For QueryOption_foobar
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_enumerator.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::numeric_limits;

namespace dps = ::mongo::dotted_path_support;

// Copied verbatim from db/index.h
static bool isIdIndex(const BSONObj& pattern) {
    BSONObjIterator i(pattern);
    BSONElement e = i.next();
    //_id index must have form exactly {_id : 1} or {_id : -1}.
    // Allows an index of form {_id : "hashed"} to exist but
    // do not consider it to be the primary _id index
    if (!(strcmp(e.fieldName(), "_id") == 0 && (e.numberInt() == 1 || e.numberInt() == -1)))
        return false;
    return i.next().eoo();
}

static bool is2DIndex(const BSONObj& pattern) {
    BSONObjIterator it(pattern);
    while (it.more()) {
        BSONElement e = it.next();
        if (String == e.type() && str::equals("2d", e.valuestr())) {
            return true;
        }
    }
    return false;
}

string optionString(size_t options) {
    mongoutils::str::stream ss;

    // These options are all currently mutually exclusive.
    if (QueryPlannerParams::DEFAULT == options) {
        ss << "DEFAULT ";
    }
    if (options & QueryPlannerParams::NO_TABLE_SCAN) {
        ss << "NO_TABLE_SCAN ";
    }
    if (options & QueryPlannerParams::INCLUDE_COLLSCAN) {
        ss << "INCLUDE_COLLSCAN ";
    }
    if (options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        ss << "INCLUDE_SHARD_FILTER ";
    }
    if (options & QueryPlannerParams::NO_BLOCKING_SORT) {
        ss << "NO_BLOCKING_SORT ";
    }
    if (options & QueryPlannerParams::INDEX_INTERSECTION) {
        ss << "INDEX_INTERSECTION ";
    }
    if (options & QueryPlannerParams::KEEP_MUTATIONS) {
        ss << "KEEP_MUTATIONS";
    }

    return ss;
}

static BSONObj getKeyFromQuery(const BSONObj& keyPattern, const BSONObj& query) {
    return query.extractFieldsUnDotted(keyPattern);
}

static bool indexCompatibleMaxMin(const BSONObj& obj, const BSONObj& keyPattern) {
    BSONObjIterator kpIt(keyPattern);
    BSONObjIterator objIt(obj);

    for (;;) {
        // Every element up to this point has matched so the KP matches
        if (!kpIt.more() && !objIt.more()) {
            return true;
        }

        // If only one iterator is done, it's not a match.
        if (!kpIt.more() || !objIt.more()) {
            return false;
        }

        // Field names must match and be in the same order.
        BSONElement kpElt = kpIt.next();
        BSONElement objElt = objIt.next();
        if (!mongoutils::str::equals(kpElt.fieldName(), objElt.fieldName())) {
            return false;
        }
    }
}

static BSONObj stripFieldNames(const BSONObj& obj) {
    BSONObjIterator it(obj);
    BSONObjBuilder bob;
    while (it.more()) {
        bob.appendAs(it.next(), "");
    }
    return bob.obj();
}

/**
 * "Finishes" the min object for the $min query option by filling in an empty object with
 * MinKey/MaxKey and stripping field names.
 *
 * In the case that 'minObj' is empty, we "finish" it by filling in either MinKey or MaxKey
 * instead. Choosing whether to use MinKey or MaxKey is done by comparing against 'maxObj'.
 * For instance, suppose 'minObj' is empty, 'maxObj' is { a: 3 }, and the key pattern is
 * { a: -1 }. According to the key pattern ordering, { a: 3 } < MinKey. This means that the
 * proper resulting bounds are
 *
 *   start: { '': MaxKey }, end: { '': 3 }
 *
 * as opposed to
 *
 *   start: { '': MinKey }, end: { '': 3 }
 *
 * Suppose instead that the key pattern is { a: 1 }, with the same 'minObj' and 'maxObj'
 * (that is, an empty object and { a: 3 } respectively). In this case, { a: 3 } > MinKey,
 * which means that we use range [{'': MinKey}, {'': 3}]. The proper 'minObj' in this case is
 * MinKey, whereas in the previous example it was MaxKey.
 *
 * If 'minObj' is non-empty, then all we do is strip its field names (because index keys always
 * have empty field names).
 */
static BSONObj finishMinObj(const BSONObj& kp, const BSONObj& minObj, const BSONObj& maxObj) {
    BSONObjBuilder bob;
    bob.appendMinKey("");
    BSONObj minKey = bob.obj();

    if (minObj.isEmpty()) {
        if (0 > minKey.woCompare(maxObj, kp, false)) {
            BSONObjBuilder minKeyBuilder;
            minKeyBuilder.appendMinKey("");
            return minKeyBuilder.obj();
        } else {
            BSONObjBuilder maxKeyBuilder;
            maxKeyBuilder.appendMaxKey("");
            return maxKeyBuilder.obj();
        }
    } else {
        return stripFieldNames(minObj);
    }
}

/**
 * "Finishes" the max object for the $max query option by filling in an empty object with
 * MinKey/MaxKey and stripping field names.
 *
 * See comment for finishMinObj() for why we need both 'minObj' and 'maxObj'.
 */
static BSONObj finishMaxObj(const BSONObj& kp, const BSONObj& minObj, const BSONObj& maxObj) {
    BSONObjBuilder bob;
    bob.appendMaxKey("");
    BSONObj maxKey = bob.obj();

    if (maxObj.isEmpty()) {
        if (0 < maxKey.woCompare(minObj, kp, false)) {
            BSONObjBuilder maxKeyBuilder;
            maxKeyBuilder.appendMaxKey("");
            return maxKeyBuilder.obj();
        } else {
            BSONObjBuilder minKeyBuilder;
            minKeyBuilder.appendMinKey("");
            return minKeyBuilder.obj();
        }
    } else {
        return stripFieldNames(maxObj);
    }
}

QuerySolution* buildCollscanSoln(const CanonicalQuery& query,
                                 bool tailable,
                                 const QueryPlannerParams& params) {
    QuerySolutionNode* solnRoot = QueryPlannerAccess::makeCollectionScan(query, tailable, params);
    return QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
}

QuerySolution* buildWholeIXSoln(const IndexEntry& index,
                                const CanonicalQuery& query,
                                const QueryPlannerParams& params,
                                int direction = 1) {
    QuerySolutionNode* solnRoot =
        QueryPlannerAccess::scanWholeIndex(index, query, params, direction);
    return QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
}

bool providesSort(const CanonicalQuery& query, const BSONObj& kp) {
    return query.getQueryRequest().getSort().isPrefixOf(kp);
}

// static
const int QueryPlanner::kPlannerVersion = 1;

Status QueryPlanner::cacheDataFromTaggedTree(const MatchExpression* const taggedTree,
                                             const vector<IndexEntry>& relevantIndices,
                                             PlanCacheIndexTree** out) {
    // On any early return, the out-parameter must contain NULL.
    *out = NULL;

    if (NULL == taggedTree) {
        return Status(ErrorCodes::BadValue, "Cannot produce cache data: tree is NULL.");
    }

    unique_ptr<PlanCacheIndexTree> indexTree(new PlanCacheIndexTree());

    if (NULL != taggedTree->getTag()) {
        IndexTag* itag = static_cast<IndexTag*>(taggedTree->getTag());
        if (itag->index >= relevantIndices.size()) {
            mongoutils::str::stream ss;
            ss << "Index number is " << itag->index << " but there are only "
               << relevantIndices.size() << " relevant indices.";
            return Status(ErrorCodes::BadValue, ss);
        }

        // Make sure not to cache solutions which use '2d' indices.
        // A 2d index that doesn't wrap on one query may wrap on another, so we have to
        // check that the index is OK with the predicate. The only thing we have to do
        // this for is 2d.  For now it's easier to move ahead if we don't cache 2d.
        //
        // TODO: revisit with a post-cached-index-assignment compatibility check
        if (is2DIndex(relevantIndices[itag->index].keyPattern)) {
            return Status(ErrorCodes::BadValue, "can't cache '2d' index");
        }

        IndexEntry* ientry = new IndexEntry(relevantIndices[itag->index]);
        indexTree->entry.reset(ientry);
        indexTree->index_pos = itag->pos;
        indexTree->canCombineBounds = itag->canCombineBounds;
    }

    for (size_t i = 0; i < taggedTree->numChildren(); ++i) {
        MatchExpression* taggedChild = taggedTree->getChild(i);
        PlanCacheIndexTree* indexTreeChild;
        Status s = cacheDataFromTaggedTree(taggedChild, relevantIndices, &indexTreeChild);
        if (!s.isOK()) {
            return s;
        }
        indexTree->children.push_back(indexTreeChild);
    }

    *out = indexTree.release();
    return Status::OK();
}

// static
Status QueryPlanner::tagAccordingToCache(MatchExpression* filter,
                                         const PlanCacheIndexTree* const indexTree,
                                         const map<BSONObj, size_t>& indexMap) {
    if (NULL == filter) {
        return Status(ErrorCodes::BadValue, "Cannot tag tree: filter is NULL.");
    }
    if (NULL == indexTree) {
        return Status(ErrorCodes::BadValue, "Cannot tag tree: indexTree is NULL.");
    }

    // We're tagging the tree here, so it shouldn't have
    // any tags hanging off yet.
    verify(NULL == filter->getTag());

    if (filter->numChildren() != indexTree->children.size()) {
        mongoutils::str::stream ss;
        ss << "Cache topology and query did not match: "
           << "query has " << filter->numChildren() << " children "
           << "and cache has " << indexTree->children.size() << " children.";
        return Status(ErrorCodes::BadValue, ss);
    }

    // Continue the depth-first tree traversal.
    for (size_t i = 0; i < filter->numChildren(); ++i) {
        Status s = tagAccordingToCache(filter->getChild(i), indexTree->children[i], indexMap);
        if (!s.isOK()) {
            return s;
        }
    }

    if (NULL != indexTree->entry.get()) {
        map<BSONObj, size_t>::const_iterator got = indexMap.find(indexTree->entry->keyPattern);
        if (got == indexMap.end()) {
            mongoutils::str::stream ss;
            ss << "Did not find index with keyPattern: " << indexTree->entry->keyPattern.toString();
            return Status(ErrorCodes::BadValue, ss);
        }
        filter->setTag(
            new IndexTag(got->second, indexTree->index_pos, indexTree->canCombineBounds));
    }

    return Status::OK();
}

// static
Status QueryPlanner::planFromCache(const CanonicalQuery& query,
                                   const QueryPlannerParams& params,
                                   const CachedSolution& cachedSoln,
                                   QuerySolution** out) {
    invariant(!cachedSoln.plannerData.empty());
    invariant(out);

    // A query not suitable for caching should not have made its way into the cache.
    invariant(PlanCache::shouldCacheQuery(query));

    // Look up winning solution in cached solution's array.
    const SolutionCacheData& winnerCacheData = *cachedSoln.plannerData[0];

    if (SolutionCacheData::WHOLE_IXSCAN_SOLN == winnerCacheData.solnType) {
        // The solution can be constructed by a scan over the entire index.
        QuerySolution* soln = buildWholeIXSoln(
            *winnerCacheData.tree->entry, query, params, winnerCacheData.wholeIXSolnDir);
        if (soln == NULL) {
            return Status(ErrorCodes::BadValue,
                          "plan cache error: soln that uses index to provide sort");
        } else {
            *out = soln;
            return Status::OK();
        }
    } else if (SolutionCacheData::COLLSCAN_SOLN == winnerCacheData.solnType) {
        // The cached solution is a collection scan. We don't cache collscans
        // with tailable==true, hence the false below.
        QuerySolution* soln = buildCollscanSoln(query, false, params);
        if (soln == NULL) {
            return Status(ErrorCodes::BadValue, "plan cache error: collection scan soln");
        } else {
            *out = soln;
            return Status::OK();
        }
    }

    // SolutionCacheData::USE_TAGS_SOLN == cacheData->solnType
    // If we're here then this is neither the whole index scan or collection scan
    // cases, and we proceed by using the PlanCacheIndexTree to tag the query tree.

    // Create a copy of the expression tree.  We use cachedSoln to annotate this with indices.
    unique_ptr<MatchExpression> clone = query.root()->shallowClone();

    LOG(5) << "Tagging the match expression according to cache data: " << endl
           << "Filter:" << endl
           << clone->toString() << "Cache data:" << endl
           << winnerCacheData.toString();

    // Map from index name to index number.
    // TODO: can we assume that the index numbering has the same lifetime
    // as the cache state?
    map<BSONObj, size_t> indexMap;
    for (size_t i = 0; i < params.indices.size(); ++i) {
        const IndexEntry& ie = params.indices[i];
        indexMap[ie.keyPattern] = i;
        LOG(5) << "Index " << i << ": " << ie.keyPattern.toString() << endl;
    }

    Status s = tagAccordingToCache(clone.get(), winnerCacheData.tree.get(), indexMap);
    if (!s.isOK()) {
        return s;
    }

    // The planner requires a defined sort order.
    sortUsingTags(clone.get());

    LOG(5) << "Tagged tree:" << endl << clone->toString();

    // Use the cached index assignments to build solnRoot.
    QuerySolutionNode* solnRoot = QueryPlannerAccess::buildIndexedDataAccess(
        query, clone.release(), false, params.indices, params);

    if (!solnRoot) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Failed to create data access plan from cache. Query: "
                                    << query.toStringShort());
    }

    // Takes ownership of 'solnRoot'.
    QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
    if (!soln) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Failed to analyze plan from cache. Query: "
                                    << query.toStringShort());
    }

    LOG(5) << "Planner: solution constructed from the cache:\n" << soln->toString();
    *out = soln;
    return Status::OK();
}

// static
Status QueryPlanner::plan(const CanonicalQuery& query,
                          const QueryPlannerParams& params,
                          std::vector<QuerySolution*>* out) {
    LOG(5) << "Beginning planning..." << endl
           << "=============================" << endl
           << "Options = " << optionString(params.options) << endl
           << "Canonical query:" << endl
           << query.toString() << "=============================" << endl;

    for (size_t i = 0; i < params.indices.size(); ++i) {
        LOG(5) << "Index " << i << " is " << params.indices[i].toString() << endl;
    }

    const bool canTableScan = !(params.options & QueryPlannerParams::NO_TABLE_SCAN);
    const bool isTailable = query.getQueryRequest().isTailable();

    // If the query requests a tailable cursor, the only solution is a collscan + filter with
    // tailable set on the collscan.  TODO: This is a policy departure.  Previously I think you
    // could ask for a tailable cursor and it just tried to give you one.  Now, we fail if we
    // can't provide one.  Is this what we want?
    if (isTailable) {
        if (!QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) && canTableScan) {
            QuerySolution* soln = buildCollscanSoln(query, isTailable, params);
            if (NULL != soln) {
                out->push_back(soln);
            }
        }
        return Status::OK();
    }

    // The hint or sort can be $natural: 1.  If this happens, output a collscan. If both
    // a $natural hint and a $natural sort are specified, then the direction of the collscan
    // is determined by the sign of the sort (not the sign of the hint).
    if (!query.getQueryRequest().getHint().isEmpty() ||
        !query.getQueryRequest().getSort().isEmpty()) {
        BSONObj hintObj = query.getQueryRequest().getHint();
        BSONObj sortObj = query.getQueryRequest().getSort();
        BSONElement naturalHint = dps::extractElementAtPath(hintObj, "$natural");
        BSONElement naturalSort = dps::extractElementAtPath(sortObj, "$natural");

        // A hint overrides a $natural sort. This means that we don't force a table
        // scan if there is a $natural sort with a non-$natural hint.
        if (!naturalHint.eoo() || (!naturalSort.eoo() && hintObj.isEmpty())) {
            LOG(5) << "Forcing a table scan due to hinted $natural\n";
            // min/max are incompatible with $natural.
            if (canTableScan && query.getQueryRequest().getMin().isEmpty() &&
                query.getQueryRequest().getMax().isEmpty()) {
                QuerySolution* soln = buildCollscanSoln(query, isTailable, params);
                if (NULL != soln) {
                    out->push_back(soln);
                }
            }
            return Status::OK();
        }
    }

    // Figure out what fields we care about.
    unordered_set<string> fields;
    QueryPlannerIXSelect::getFields(query.root(), "", &fields);

    for (unordered_set<string>::const_iterator it = fields.begin(); it != fields.end(); ++it) {
        LOG(5) << "Predicate over field '" << *it << "'" << endl;
    }

    // Filter our indices so we only look at indices that are over our predicates.
    vector<IndexEntry> relevantIndices;

    // Hints require us to only consider the hinted index.
    // If index filters in the query settings were used to override
    // the allowed indices for planning, we should not use the hinted index
    // requested in the query.
    BSONObj hintIndex;
    if (!params.indexFiltersApplied) {
        hintIndex = query.getQueryRequest().getHint();
    }

    // If snapshot is set, default to collscanning. If the query param SNAPSHOT_USE_ID is set,
    // snapshot is a form of a hint, so try to use _id index to make a real plan. If that fails,
    // just scan the _id index.
    //
    // Don't do this if the query is a geonear or text as as text search queries must be answered
    // using full text indices and geoNear queries must be answered using geospatial indices.
    if (query.getQueryRequest().isSnapshot() &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)) {
        const bool useIXScan = params.options & QueryPlannerParams::SNAPSHOT_USE_ID;

        if (!useIXScan) {
            QuerySolution* soln = buildCollscanSoln(query, isTailable, params);
            if (soln) {
                out->push_back(soln);
            }
            return Status::OK();
        } else {
            // Find the ID index in indexKeyPatterns. It's our hint.
            for (size_t i = 0; i < params.indices.size(); ++i) {
                if (isIdIndex(params.indices[i].keyPattern)) {
                    hintIndex = params.indices[i].keyPattern;
                    break;
                }
            }
        }
    }

    size_t hintIndexNumber = numeric_limits<size_t>::max();

    if (hintIndex.isEmpty()) {
        QueryPlannerIXSelect::findRelevantIndices(fields, params.indices, &relevantIndices);
    } else {
        // Sigh.  If the hint is specified it might be using the index name.
        BSONElement firstHintElt = hintIndex.firstElement();
        if (str::equals("$hint", firstHintElt.fieldName()) && String == firstHintElt.type()) {
            string hintName = firstHintElt.String();
            for (size_t i = 0; i < params.indices.size(); ++i) {
                if (params.indices[i].name == hintName) {
                    LOG(5) << "Hint by name specified, restricting indices to "
                           << params.indices[i].keyPattern.toString() << endl;
                    relevantIndices.clear();
                    relevantIndices.push_back(params.indices[i]);
                    hintIndexNumber = i;
                    hintIndex = params.indices[i].keyPattern;
                    break;
                }
            }
        } else {
            for (size_t i = 0; i < params.indices.size(); ++i) {
                if (0 == params.indices[i].keyPattern.woCompare(hintIndex)) {
                    relevantIndices.clear();
                    relevantIndices.push_back(params.indices[i]);
                    LOG(5) << "Hint specified, restricting indices to " << hintIndex.toString()
                           << endl;
                    hintIndexNumber = i;
                    break;
                }
            }
        }

        if (hintIndexNumber == numeric_limits<size_t>::max()) {
            return Status(ErrorCodes::BadValue, "bad hint");
        }
    }

    // Deal with the .min() and .max() query options.  If either exist we can only use an index
    // that matches the object inside.
    if (!query.getQueryRequest().getMin().isEmpty() ||
        !query.getQueryRequest().getMax().isEmpty()) {
        BSONObj minObj = query.getQueryRequest().getMin();
        BSONObj maxObj = query.getQueryRequest().getMax();

        // The unfinished siblings of these objects may not be proper index keys because they
        // may be empty objects or have field names. When an index is picked to use for the
        // min/max query, these "finished" objects will always be valid index keys for the
        // index's key pattern.
        BSONObj finishedMinObj;
        BSONObj finishedMaxObj;

        // This is the index into params.indices[...] that we use.
        size_t idxNo = numeric_limits<size_t>::max();

        // If there's an index hinted we need to be able to use it.
        if (!hintIndex.isEmpty()) {
            if (!minObj.isEmpty() && !indexCompatibleMaxMin(minObj, hintIndex)) {
                LOG(5) << "Minobj doesn't work with hint";
                return Status(ErrorCodes::BadValue, "hint provided does not work with min query");
            }

            if (!maxObj.isEmpty() && !indexCompatibleMaxMin(maxObj, hintIndex)) {
                LOG(5) << "Maxobj doesn't work with hint";
                return Status(ErrorCodes::BadValue, "hint provided does not work with max query");
            }

            const BSONObj& kp = params.indices[hintIndexNumber].keyPattern;
            finishedMinObj = finishMinObj(kp, minObj, maxObj);
            finishedMaxObj = finishMaxObj(kp, minObj, maxObj);

            // The min must be less than the max for the hinted index ordering.
            if (0 <= finishedMinObj.woCompare(finishedMaxObj, kp, false)) {
                LOG(5) << "Minobj/Maxobj don't work with hint";
                return Status(ErrorCodes::BadValue,
                              "hint provided does not work with min/max query");
            }

            idxNo = hintIndexNumber;
        } else {
            // No hinted index, look for one that is compatible (has same field names and
            // ordering thereof).
            for (size_t i = 0; i < params.indices.size(); ++i) {
                const BSONObj& kp = params.indices[i].keyPattern;

                BSONObj toUse = minObj.isEmpty() ? maxObj : minObj;
                if (indexCompatibleMaxMin(toUse, kp)) {
                    // In order to be fully compatible, the min has to be less than the max
                    // according to the index key pattern ordering. The first step in verifying
                    // this is "finish" the min and max by replacing empty objects and stripping
                    // field names.
                    finishedMinObj = finishMinObj(kp, minObj, maxObj);
                    finishedMaxObj = finishMaxObj(kp, minObj, maxObj);

                    // Now we have the final min and max. This index is only relevant for
                    // the min/max query if min < max.
                    if (0 >= finishedMinObj.woCompare(finishedMaxObj, kp, false)) {
                        // Found a relevant index.
                        idxNo = i;
                        break;
                    }

                    // This index is not relevant; move on to the next.
                }
            }
        }

        if (idxNo == numeric_limits<size_t>::max()) {
            LOG(5) << "Can't find relevant index to use for max/min query";
            // Can't find an index to use, bail out.
            return Status(ErrorCodes::BadValue, "unable to find relevant index for max/min query");
        }

        LOG(5) << "Max/min query using index " << params.indices[idxNo].toString() << endl;

        // Make our scan and output.
        QuerySolutionNode* solnRoot = QueryPlannerAccess::makeIndexScan(
            params.indices[idxNo], query, params, finishedMinObj, finishedMaxObj);

        QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
        if (NULL != soln) {
            out->push_back(soln);
        }

        return Status::OK();
    }

    for (size_t i = 0; i < relevantIndices.size(); ++i) {
        LOG(2) << "Relevant index " << i << " is " << relevantIndices[i].toString() << endl;
    }

    // Figure out how useful each index is to each predicate.
    QueryPlannerIXSelect::rateIndices(query.root(), "", relevantIndices, query.getCollator());
    QueryPlannerIXSelect::stripInvalidAssignments(query.root(), relevantIndices);

    // Unless we have GEO_NEAR, TEXT, or a projection, we may be able to apply an optimization
    // in which we strip unnecessary index assignments.
    //
    // Disallowed with projection because assignment to a non-unique index can allow the plan
    // to be covered.
    //
    // TEXT and GEO_NEAR are special because they require the use of a text/geo index in order
    // to be evaluated correctly. Stripping these "mandatory assignments" is therefore invalid.
    if (query.getQueryRequest().getProj().isEmpty() &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)) {
        QueryPlannerIXSelect::stripUnneededAssignments(query.root(), relevantIndices);
    }

    // query.root() is now annotated with RelevantTag(s).
    LOG(5) << "Rated tree:" << endl << query.root()->toString();

    // If there is a GEO_NEAR it must have an index it can use directly.
    const MatchExpression* gnNode = NULL;
    if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR, &gnNode)) {
        // No index for GEO_NEAR?  No query.
        RelevantTag* tag = static_cast<RelevantTag*>(gnNode->getTag());
        if (!tag || (0 == tag->first.size() && 0 == tag->notFirst.size())) {
            LOG(5) << "Unable to find index for $geoNear query." << endl;
            // Don't leave tags on query tree.
            query.root()->resetTag();
            return Status(ErrorCodes::BadValue, "unable to find index for $geoNear query");
        }

        LOG(5) << "Rated tree after geonear processing:" << query.root()->toString();
    }

    // Likewise, if there is a TEXT it must have an index it can use directly.
    const MatchExpression* textNode = NULL;
    if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT, &textNode)) {
        RelevantTag* tag = static_cast<RelevantTag*>(textNode->getTag());

        // Exactly one text index required for TEXT.  We need to check this explicitly because
        // the text stage can't be built if no text index exists or there is an ambiguity as to
        // which one to use.
        size_t textIndexCount = 0;
        for (size_t i = 0; i < params.indices.size(); i++) {
            if (INDEX_TEXT == params.indices[i].type) {
                textIndexCount++;
            }
        }
        if (textIndexCount != 1) {
            // Don't leave tags on query tree.
            query.root()->resetTag();
            return Status(ErrorCodes::BadValue, "need exactly one text index for $text query");
        }

        // Error if the text node is tagged with zero indices.
        if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
            // Don't leave tags on query tree.
            query.root()->resetTag();
            return Status(ErrorCodes::BadValue,
                          "failed to use text index to satisfy $text query (if text index is "
                          "compound, are equality predicates given for all prefix fields?)");
        }

        // At this point, we know that there is only one text index and that the TEXT node is
        // assigned to it.
        invariant(1 == tag->first.size() + tag->notFirst.size());

        LOG(5) << "Rated tree after text processing:" << query.root()->toString();
    }

    // If we have any relevant indices, we try to create indexed plans.
    if (0 < relevantIndices.size()) {
        // The enumerator spits out trees tagged with IndexTag(s).
        PlanEnumeratorParams enumParams;
        enumParams.intersect = params.options & QueryPlannerParams::INDEX_INTERSECTION;
        enumParams.root = query.root();
        enumParams.indices = &relevantIndices;

        PlanEnumerator isp(enumParams);
        isp.init();

        MatchExpression* rawTree;
        while (isp.getNext(&rawTree) && (out->size() < params.maxIndexedSolutions)) {
            LOG(5) << "About to build solntree from tagged tree:" << endl << rawTree->toString();

            // The tagged tree produced by the plan enumerator is not guaranteed
            // to be canonically sorted. In order to be compatible with the cached
            // data, sort the tagged tree according to CanonicalQuery ordering.
            std::unique_ptr<MatchExpression> clone(rawTree->shallowClone());
            CanonicalQuery::sortTree(clone.get());

            PlanCacheIndexTree* cacheData;
            Status indexTreeStatus =
                cacheDataFromTaggedTree(clone.get(), relevantIndices, &cacheData);
            if (!indexTreeStatus.isOK()) {
                LOG(5) << "Query is not cachable: " << indexTreeStatus.reason() << endl;
            }
            unique_ptr<PlanCacheIndexTree> autoData(cacheData);

            // This can fail if enumeration makes a mistake.
            QuerySolutionNode* solnRoot = QueryPlannerAccess::buildIndexedDataAccess(
                query, rawTree, false, relevantIndices, params);

            if (NULL == solnRoot) {
                continue;
            }

            QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
            if (NULL != soln) {
                LOG(5) << "Planner: adding solution:" << endl << soln->toString();
                if (indexTreeStatus.isOK()) {
                    SolutionCacheData* scd = new SolutionCacheData();
                    scd->tree.reset(autoData.release());
                    soln->cacheData.reset(scd);
                }
                out->push_back(soln);
            }
        }
    }

    // Don't leave tags on query tree.
    query.root()->resetTag();

    LOG(5) << "Planner: outputted " << out->size() << " indexed solutions.\n";

    // Produce legible error message for failed OR planning with a TEXT child.
    // TODO: support collection scan for non-TEXT children of OR.
    if (out->size() == 0 && textNode != NULL && MatchExpression::OR == query.root()->matchType()) {
        MatchExpression* root = query.root();
        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (textNode == root->getChild(i)) {
                return Status(ErrorCodes::BadValue,
                              "Failed to produce a solution for TEXT under OR - "
                              "other non-TEXT clauses under OR have to be indexed as well.");
            }
        }
    }

    // An index was hinted.  If there are any solutions, they use the hinted index.  If not, we
    // scan the entire index to provide results and output that as our plan.  This is the
    // desired behavior when an index is hinted that is not relevant to the query.
    if (!hintIndex.isEmpty()) {
        if (0 == out->size()) {
            QuerySolution* soln = buildWholeIXSoln(params.indices[hintIndexNumber], query, params);
            verify(NULL != soln);
            LOG(5) << "Planner: outputting soln that uses hinted index as scan." << endl;
            out->push_back(soln);
        }
        return Status::OK();
    }

    // If a sort order is requested, there may be an index that provides it, even if that
    // index is not over any predicates in the query.
    //
    if (!query.getQueryRequest().getSort().isEmpty() &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)) {
        // See if we have a sort provided from an index already.
        // This is implied by the presence of a non-blocking solution.
        bool usingIndexToSort = false;
        for (size_t i = 0; i < out->size(); ++i) {
            QuerySolution* soln = (*out)[i];
            if (!soln->hasBlockingStage) {
                usingIndexToSort = true;
                break;
            }
        }

        if (!usingIndexToSort) {
            for (size_t i = 0; i < params.indices.size(); ++i) {
                const IndexEntry& index = params.indices[i];
                // Only regular (non-plugin) indexes can be used to provide a sort, and only
                // non-sparse indexes can be used to provide a sort.
                //
                // TODO: Sparse indexes can't normally provide a sort, because non-indexed
                // documents could potentially be missing from the result set.  However, if the
                // query predicate can be used to guarantee that all documents to be returned
                // are indexed, then the index should be able to provide the sort.
                //
                // For example:
                // - Sparse index {a: 1, b: 1} should be able to provide a sort for
                //   find({b: 1}).sort({a: 1}).  SERVER-13908.
                // - Index {a: 1, b: "2dsphere"} (which is "geo-sparse", if
                //   2dsphereIndexVersion=2) should be able to provide a sort for
                //   find({b: GEO}).sort({a:1}).  SERVER-10801.
                if (index.type != INDEX_BTREE) {
                    continue;
                }
                if (index.sparse) {
                    continue;
                }

                // If the index collation differs from the query collation, the index should not be
                // used to provide a sort, because strings will be ordered incorrectly.
                if (!CollatorInterface::collatorsMatch(index.collator, query.getCollator())) {
                    continue;
                }

                // Partial indexes can only be used to provide a sort only if the query predicate is
                // compatible.
                if (index.filterExpr && !expression::isSubsetOf(query.root(), index.filterExpr)) {
                    continue;
                }

                const BSONObj kp = QueryPlannerAnalysis::getSortPattern(index.keyPattern);
                if (providesSort(query, kp)) {
                    LOG(5) << "Planner: outputting soln that uses index to provide sort." << endl;
                    QuerySolution* soln = buildWholeIXSoln(params.indices[i], query, params);
                    if (NULL != soln) {
                        PlanCacheIndexTree* indexTree = new PlanCacheIndexTree();
                        indexTree->setIndexEntry(params.indices[i]);
                        SolutionCacheData* scd = new SolutionCacheData();
                        scd->tree.reset(indexTree);
                        scd->solnType = SolutionCacheData::WHOLE_IXSCAN_SOLN;
                        scd->wholeIXSolnDir = 1;

                        soln->cacheData.reset(scd);
                        out->push_back(soln);
                        break;
                    }
                }
                if (providesSort(query, QueryPlannerCommon::reverseSortObj(kp))) {
                    LOG(5) << "Planner: outputting soln that uses (reverse) index "
                           << "to provide sort." << endl;
                    QuerySolution* soln = buildWholeIXSoln(params.indices[i], query, params, -1);
                    if (NULL != soln) {
                        PlanCacheIndexTree* indexTree = new PlanCacheIndexTree();
                        indexTree->setIndexEntry(params.indices[i]);
                        SolutionCacheData* scd = new SolutionCacheData();
                        scd->tree.reset(indexTree);
                        scd->solnType = SolutionCacheData::WHOLE_IXSCAN_SOLN;
                        scd->wholeIXSolnDir = -1;

                        soln->cacheData.reset(scd);
                        out->push_back(soln);
                        break;
                    }
                }
            }
        }
    }

    // geoNear and text queries *require* an index.
    // Also, if a hint is specified it indicates that we MUST use it.
    bool possibleToCollscan =
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR) &&
        !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT) && hintIndex.isEmpty();

    // The caller can explicitly ask for a collscan.
    bool collscanRequested = (params.options & QueryPlannerParams::INCLUDE_COLLSCAN);

    // No indexed plans?  We must provide a collscan if possible or else we can't run the query.
    bool collscanNeeded = (0 == out->size() && canTableScan);

    if (possibleToCollscan && (collscanRequested || collscanNeeded)) {
        QuerySolution* collscan = buildCollscanSoln(query, isTailable, params);
        if (NULL != collscan) {
            SolutionCacheData* scd = new SolutionCacheData();
            scd->solnType = SolutionCacheData::COLLSCAN_SOLN;
            collscan->cacheData.reset(scd);
            out->push_back(collscan);
            LOG(5) << "Planner: outputting a collscan:" << endl << collscan->toString();
        }
    }

    return Status::OK();
}

}  // namespace mongo
