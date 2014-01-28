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

#include "mongo/db/query/query_planner.h"

#include <vector>

#include "mongo/client/dbclientinterface.h"   // For QueryOption_foobar
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/plan_enumerator.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    // Copied verbatim from db/index.h
    static bool isIdIndex( const BSONObj &pattern ) {
        BSONObjIterator i(pattern);
        BSONElement e = i.next();
        //_id index must have form exactly {_id : 1} or {_id : -1}.
        //Allows an index of form {_id : "hashed"} to exist but
        //do not consider it to be the primary _id index
        if(! ( strcmp(e.fieldName(), "_id") == 0
                && (e.numberInt() == 1 || e.numberInt() == -1)))
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
            ss << "NO_BLOCKING_SORT";
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

        QuerySolutionNode* solnRoot = QueryPlannerAccess::scanWholeIndex(index, query, params, direction);
        return QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
    }

    bool providesSort(const CanonicalQuery& query, const BSONObj& kp) {
        BSONObjIterator sortIt(query.getParsed().getSort());
        BSONObjIterator kpIt(kp);

        while (sortIt.more() && kpIt.more()) {
            // We want the field name to be the same as well (so we pass true).
            // TODO: see if we can pull a reverse sort out...
            if (0 != sortIt.next().woCompare(kpIt.next(), true)) {
                return false;
            }
        }

        // every elt in sort matched kp
        return !sortIt.more();
    }

    Status QueryPlanner::cacheDataFromTaggedTree(const MatchExpression* const taggedTree,
                                                 const vector<IndexEntry>& relevantIndices,
                                                 PlanCacheIndexTree** out) {
        // On any early return, the out-parameter must contain NULL.
        *out = NULL;

        if (NULL == taggedTree) {
            return Status(ErrorCodes::BadValue, "Cannot produce cache data: tree is NULL.");
        }

        auto_ptr<PlanCacheIndexTree> indexTree(new PlanCacheIndexTree());

        if (NULL != taggedTree->getTag()) {
            IndexTag* itag = static_cast<IndexTag*>(taggedTree->getTag());
            if (itag->index >= relevantIndices.size()) {
                mongoutils::str::stream ss;
                ss << "Index number is " << itag->index
                   << " but there are only " << relevantIndices.size()
                   << " relevant indices.";
                return Status(ErrorCodes::BadValue, ss);
            }

            // Make sure not to cache solutions which use '2d' indices.
            // A 2d index that doesn't wrap on one query may wrap on another, so we have to
            // check that the index is OK with the predicate. The only thing we have to do
            // this for is 2d.  For now it's easier to move ahead if we don't cache 2d.
            //
            // XXX: revisit with a post-cached-index-assignment compatibility check
            if (is2DIndex(relevantIndices[itag->index].keyPattern)) {
                return Status(ErrorCodes::BadValue, "can't cache '2d' index");
            }

            IndexEntry* ientry = new IndexEntry(relevantIndices[itag->index]);
            indexTree->entry.reset(ientry);
            indexTree->index_pos = itag->pos;
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
            filter->setTag(new IndexTag(got->second, indexTree->index_pos));
        }

        return Status::OK();
    }

    // static
    Status QueryPlanner::planFromCache(const CanonicalQuery& query,
                                       const QueryPlannerParams& params,
                                       const SolutionCacheData& cacheData,
                                       QuerySolution** out) {
        if (SolutionCacheData::WHOLE_IXSCAN_SOLN == cacheData.solnType) {
            // The solution can be constructed by a scan over the entire index.
            QuerySolution* soln = buildWholeIXSoln(*cacheData.tree->entry,
                query, params, cacheData.wholeIXSolnDir);
            if (soln == NULL) {
                return Status(ErrorCodes::BadValue,
                              "plan cache error: soln that uses index to provide sort");
            }
            else {
                *out = soln;
                return Status::OK();
            }
        }
        else if (SolutionCacheData::COLLSCAN_SOLN == cacheData.solnType) {
            // The cached solution is a collection scan. We don't cache collscans
            // with tailable==true, hence the false below.
            QuerySolution* soln = buildCollscanSoln(query, false, params);
            if (soln == NULL) {
                return Status(ErrorCodes::BadValue, "plan cache error: collection scan soln");
            }
            else {
                *out = soln;
                return Status::OK();
            }
        }

        // SolutionCacheData::USE_TAGS_SOLN == cacheData->solnType
        // If we're here then this is neither the whole index scan or collection scan
        // cases, and we proceed by using the PlanCacheIndexTree to tag the query tree.

        // Create a copy of the expression tree.  We use cachedSoln to annotate this with indices.
        MatchExpression* clone = query.root()->shallowClone();

        QLOG() << "Tagging the match expression according to cache data: " << endl
               << "Filter:" << endl << clone->toString()
               << "Cache data:" << endl << cacheData.toString();

        // Map from index name to index number.
        // TODO: can we assume that the index numbering has the same lifetime
        // as the cache state?
        map<BSONObj, size_t> indexMap;
        for (size_t i = 0; i < params.indices.size(); ++i) {
            const IndexEntry& ie = params.indices[i];
            indexMap[ie.keyPattern] = i;
            QLOG() << "Index " << i << ": " << ie.keyPattern.toString() << endl;
        }

        Status s = tagAccordingToCache(clone, cacheData.tree.get(), indexMap);
        if (!s.isOK()) {
            return s;
        }

        // The planner requires a defined sort order.
        sortUsingTags(clone);

        QLOG() << "Tagged tree:" << endl << clone->toString();

        // Use the cached index assignments to build solnRoot.  Takes ownership of clone.
        QuerySolutionNode* solnRoot =
            QueryPlannerAccess::buildIndexedDataAccess(query, clone, false, params.indices);

        if (NULL != solnRoot) {
            // Takes ownership of 'solnRoot'.
            QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
            if (NULL != soln) {
                QLOG() << "Planner: adding cached solution:\n" << soln->toString() << endl;
                *out = soln;
                return Status::OK();
            }
        }

        return Status(ErrorCodes::BadValue, "couldn't plan from cache");
    }

    // static
    Status QueryPlanner::planFromCache(const CanonicalQuery& query,
                                       const QueryPlannerParams& params,
                                       const CachedSolution& cachedSoln,
                                       QuerySolution** out,
                                       QuerySolution** backupOut) {
        verify(!cachedSoln.plannerData.empty());
        verify(out);
        verify(backupOut);
        verify(PlanCache::shouldCacheQuery(query));

        // If there is no backup solution, then return NULL through
        // the 'backupOut' out-parameter.
        *backupOut = NULL;

        // Queries not suitable for caching are filtered
        // in multi plan runner using PlanCache::shouldCacheQuery().

        // Look up winning solution in cached solution's array.
        SolutionCacheData* winnerCacheData = cachedSoln.plannerData[0];
        Status s = planFromCache(query, params, *winnerCacheData, out);
        if (!s.isOK()) {
            return s;
        }

        if (cachedSoln.backupSoln) {
            SolutionCacheData* backupCacheData = cachedSoln.plannerData[*cachedSoln.backupSoln];
            Status backupStatus = planFromCache(query, params, *backupCacheData, backupOut);
            if (!backupStatus.isOK()) {
                return backupStatus;
            }
        }

        return Status::OK();
    }

    // static
    Status QueryPlanner::plan(const CanonicalQuery& query,
                              const QueryPlannerParams& params,
                              std::vector<QuerySolution*>* out) {

        QLOG() << "=============================\n"
               << "Beginning planning, options = " << optionString(params.options) << endl
               << "Canonical query:\n" << query.toString() << endl
               << "============================="
               << endl;

        for (size_t i = 0; i < params.indices.size(); ++i) {
            QLOG() << "idx " << i << " is " << params.indices[i].toString() << endl;
        }

        bool canTableScan = !(params.options & QueryPlannerParams::NO_TABLE_SCAN);

        // If the query requests a tailable cursor, the only solution is a collscan + filter with
        // tailable set on the collscan.  TODO: This is a policy departure.  Previously I think you
        // could ask for a tailable cursor and it just tried to give you one.  Now, we fail if we
        // can't provide one.  Is this what we want?
        if (query.getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)
                && canTableScan) {
                QuerySolution* soln = buildCollscanSoln(query, true, params);
                if (NULL != soln) {
                    out->push_back(soln);
                }
            }
            return Status::OK();
        }

        // The hint can be $natural: 1.  If this happens, output a collscan.  It's a weird way of
        // saying "table scan for two, please."
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                QLOG() << "forcing a table scan due to hinted $natural\n";
                // min/max are incompatible with $natural.
                if (canTableScan && query.getParsed().getMin().isEmpty()
                                 && query.getParsed().getMax().isEmpty()) {
                    QuerySolution* soln = buildCollscanSoln(query, false, params);
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
            QLOG() << "predicate over field " << *it << endl;
        }

        // Filter our indices so we only look at indices that are over our predicates.
        vector<IndexEntry> relevantIndices;

        // Hints require us to only consider the hinted index.
        // If index filters in the query settings were used to override
        // the allowed indices for planning, we should not use the hinted index
        // requested in the query.
        BSONObj hintIndex;
        if (!params.indexFiltersApplied) {
            hintIndex = query.getParsed().getHint();
        }

        // Snapshot is a form of a hint.  If snapshot is set, try to use _id index to make a real
        // plan.  If that fails, just scan the _id index.
        if (query.getParsed().isSnapshot()) {
            // Find the ID index in indexKeyPatterns.  It's our hint.
            for (size_t i = 0; i < params.indices.size(); ++i) {
                if (isIdIndex(params.indices[i].keyPattern)) {
                    hintIndex = params.indices[i].keyPattern;
                    break;
                }
            }
        }

        size_t hintIndexNumber = numeric_limits<size_t>::max();

        if (hintIndex.isEmpty()) {
            QueryPlannerIXSelect::findRelevantIndices(fields, params.indices, &relevantIndices);
        }
        else {
            // Sigh.  If the hint is specified it might be using the index name.
            BSONElement firstHintElt = hintIndex.firstElement();
            if (str::equals("$hint", firstHintElt.fieldName()) && String == firstHintElt.type()) {
                string hintName = firstHintElt.String();
                for (size_t i = 0; i < params.indices.size(); ++i) {
                    if (params.indices[i].name == hintName) {
                        QLOG() << "hint by name specified, restricting indices to "
                             << params.indices[i].keyPattern.toString() << endl;
                        relevantIndices.clear();
                        relevantIndices.push_back(params.indices[i]);
                        hintIndexNumber = i;
                        hintIndex = params.indices[i].keyPattern;
                        break;
                    }
                }
            }
            else {
                for (size_t i = 0; i < params.indices.size(); ++i) {
                    if (0 == params.indices[i].keyPattern.woCompare(hintIndex)) {
                        relevantIndices.clear();
                        relevantIndices.push_back(params.indices[i]);
                        QLOG() << "hint specified, restricting indices to " << hintIndex.toString()
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
        if (!query.getParsed().getMin().isEmpty() || !query.getParsed().getMax().isEmpty()) {
            BSONObj minObj = query.getParsed().getMin();
            BSONObj maxObj = query.getParsed().getMax();

            // This is the index into params.indices[...] that we use.
            size_t idxNo = numeric_limits<size_t>::max();

            // If there's an index hinted we need to be able to use it.
            if (!hintIndex.isEmpty()) {
                if (!minObj.isEmpty() && !indexCompatibleMaxMin(minObj, hintIndex)) {
                    QLOG() << "minobj doesnt work w hint";
                    return Status(ErrorCodes::BadValue,
                                  "hint provided does not work with min query");
                }

                if (!maxObj.isEmpty() && !indexCompatibleMaxMin(maxObj, hintIndex)) {
                    QLOG() << "maxobj doesnt work w hint";
                    return Status(ErrorCodes::BadValue,
                                  "hint provided does not work with max query");
                }

                idxNo = hintIndexNumber;
            }
            else {
                // No hinted index, look for one that is compatible (has same field names and
                // ordering thereof).
                for (size_t i = 0; i < params.indices.size(); ++i) {
                    const BSONObj& kp = params.indices[i].keyPattern;

                    BSONObj toUse = minObj.isEmpty() ? maxObj : minObj;
                    if (indexCompatibleMaxMin(toUse, kp)) {
                        idxNo = i;
                        break;
                    }
                }
            }
            
            if (idxNo == numeric_limits<size_t>::max()) {
                QLOG() << "Can't find relevant index to use for max/min query";
                // Can't find an index to use, bail out.
                return Status(ErrorCodes::BadValue,
                              "unable to find relevant index for max/min query");
            }

            // maxObj can be empty; the index scan just goes until the end.  minObj can't be empty
            // though, so if it is, we make a minKey object.
            if (minObj.isEmpty()) {
                BSONObjBuilder bob;
                bob.appendMinKey("");
                minObj = bob.obj();
            }
            else {
                // Must strip off the field names to make an index key.
                minObj = stripFieldNames(minObj);
            }

            if (!maxObj.isEmpty()) {
                // Must strip off the field names to make an index key.
                maxObj = stripFieldNames(maxObj);
            }

            QLOG() << "max/min query using index " << params.indices[idxNo].toString() << endl;

            // Make our scan and output.
            QuerySolutionNode* solnRoot = QueryPlannerAccess::makeIndexScan(params.indices[idxNo],
                                                                            query,
                                                                            params,
                                                                            minObj,
                                                                            maxObj);

            QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
            if (NULL != soln) {
                out->push_back(soln);
            }

            return Status::OK();
        }

        for (size_t i = 0; i < relevantIndices.size(); ++i) {
            QLOG() << "relevant idx " << i << " is " << relevantIndices[i].toString() << endl;
        }

        // Figure out how useful each index is to each predicate.
        // query.root() is now annotated with RelevantTag(s).
        QueryPlannerIXSelect::rateIndices(query.root(), "", relevantIndices);

        QLOG() << "rated tree" << endl;
        QLOG() << query.root()->toString() << endl;

        // If there is a GEO_NEAR it must have an index it can use directly.
        // XXX: move into data access?
        MatchExpression* gnNode = NULL;
        if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR, &gnNode)) {
            // No index for GEO_NEAR?  No query.
            RelevantTag* tag = static_cast<RelevantTag*>(gnNode->getTag());
            if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
                QLOG() << "unable to find index for $geoNear query" << endl;
                return Status(ErrorCodes::BadValue, "unable to find index for $geoNear query");
            }

            GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(gnNode);

            vector<size_t> newFirst;

            // 2d + GEO_NEAR is annoying.  Because 2d's GEO_NEAR isn't streaming we have to embed
            // the full query tree inside it as a matcher.
            for (size_t i = 0; i < tag->first.size(); ++i) {
                // GEO_NEAR has a non-2d index it can use.  We can deal w/that in normal planning.
                if (!is2DIndex(relevantIndices[tag->first[i]].keyPattern)) {
                    newFirst.push_back(i);
                    continue;
                }

                // If we're here, GEO_NEAR has a 2d index.  We create a 2dgeonear plan with the
                // entire tree as a filter, if possible.

                GeoNear2DNode* solnRoot = new GeoNear2DNode();
                solnRoot->nq = gnme->getData();
                if (NULL != query.getProj()) {
                    solnRoot->addPointMeta = query.getProj()->wantGeoNearPoint();
                    solnRoot->addDistMeta = query.getProj()->wantGeoNearDistance();
                }

                if (MatchExpression::GEO_NEAR != query.root()->matchType()) {
                    // root is an AND, clone and delete the GEO_NEAR child.
                    MatchExpression* filterTree = query.root()->shallowClone();
                    verify(MatchExpression::AND == filterTree->matchType());

                    bool foundChild = false;
                    for (size_t i = 0; i < filterTree->numChildren(); ++i) {
                        if (MatchExpression::GEO_NEAR == filterTree->getChild(i)->matchType()) {
                            foundChild = true;
                            filterTree->getChildVector()->erase(filterTree->getChildVector()->begin() + i);
                            break;
                        }
                    }
                    verify(foundChild);
                    solnRoot->filter.reset(filterTree);
                }

                solnRoot->numWanted = query.getParsed().getNumToReturn();
                if (0 == solnRoot->numWanted) {
                    solnRoot->numWanted = 100;
                }
                solnRoot->indexKeyPattern = relevantIndices[tag->first[i]].keyPattern;

                // Remove the 2d index.  2d can only be the first field, and we know there is
                // only one GEO_NEAR, so we don't care if anyone else was assigned it; it'll
                // only be first for gnNode.
                tag->first.erase(tag->first.begin() + i);

                QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);

                if (NULL != soln) {
                    out->push_back(soln);
                }
            }

            // Continue planning w/non-2d indices tagged for this pred.
            tag->first.swap(newFirst);

            if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
                return Status::OK();
            }
        }

        // Likewise, if there is a TEXT it must have an index it can use directly.
        MatchExpression* textNode;
        if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT, &textNode)) {
            RelevantTag* tag = static_cast<RelevantTag*>(textNode->getTag());
            if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
                return Status::OK();
            }
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
            // XXX: have limit on # of indexed solns we'll consider.  We could have a perverse
            // query and index that could make n^2 very unpleasant.
            while (isp.getNext(&rawTree)) {
                QLOG() << "about to build solntree from tagged tree:\n" << rawTree->toString()
                       << endl;

                // The tagged tree produced by the plan enumerator is not guaranteed
                // to be canonically sorted. In order to be compatible with the cached
                // data, sort the tagged tree according to CanonicalQuery ordering.
                boost::scoped_ptr<MatchExpression> clone(rawTree->shallowClone());
                CanonicalQuery::sortTree(clone.get());

                PlanCacheIndexTree* cacheData;
                Status indexTreeStatus = cacheDataFromTaggedTree(clone.get(), relevantIndices, &cacheData);
                if (!indexTreeStatus.isOK()) {
                    QLOG() << "Query is not cachable: " << indexTreeStatus.reason() << endl;
                }
                auto_ptr<PlanCacheIndexTree> autoData(cacheData);

                // This can fail if enumeration makes a mistake.
                QuerySolutionNode* solnRoot =
                    QueryPlannerAccess::buildIndexedDataAccess(query, rawTree, false, relevantIndices);

                if (NULL == solnRoot) { continue; }

                QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
                if (NULL != soln) {
                    QLOG() << "Planner: adding solution:\n" << soln->toString() << endl;
                    if (indexTreeStatus.isOK()) {
                        SolutionCacheData* scd = new SolutionCacheData();
                        scd->tree.reset(autoData.release());
                        soln->cacheData.reset(scd);
                    }
                    out->push_back(soln);
                }
            }
        }

        QLOG() << "Planner: outputted " << out->size() << " indexed solutions.\n";

        // An index was hinted.  If there are any solutions, they use the hinted index.  If not, we
        // scan the entire index to provide results and output that as our plan.  This is the
        // desired behavior when an index is hinted that is not relevant to the query.
        if (!hintIndex.isEmpty()) {
            if (0 == out->size()) {
                QuerySolution* soln = buildWholeIXSoln(params.indices[hintIndexNumber], query, params);
                verify(NULL != soln);
                QLOG() << "Planner: outputting soln that uses hinted index as scan." << endl;
                out->push_back(soln);
            }
            return Status::OK();
        }

        // If a sort order is requested, there may be an index that provides it, even if that
        // index is not over any predicates in the query.
        //
        if (!query.getParsed().getSort().isEmpty()
            && !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)
            && !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)) {

            // See if we have a sort provided from an index already.
            bool usingIndexToSort = false;
            for (size_t i = 0; i < out->size(); ++i) {
                QuerySolution* soln = (*out)[i];
                if (!soln->hasSortStage) {
                    usingIndexToSort = true;
                    break;
                }
            }

            if (!usingIndexToSort) {
                for (size_t i = 0; i < params.indices.size(); ++i) {
                    const IndexEntry& index = params.indices[i];
                    if (index.sparse) {
                        continue;
                    }
                    const BSONObj kp = LiteParsedQuery::normalizeSortOrder(index.keyPattern);
                    if (providesSort(query, kp)) {
                        QLOG() << "Planner: outputting soln that uses index to provide sort."
                               << endl;
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
                        QLOG() << "Planner: outputting soln that uses (reverse) index "
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

        // TODO: Do we always want to offer a collscan solution?
        // XXX: currently disabling the always-use-a-collscan in order to find more planner bugs.
        if (    !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)
             && !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)
             && hintIndex.isEmpty()
             && ((params.options & QueryPlannerParams::INCLUDE_COLLSCAN) || (0 == out->size() && canTableScan)))
        {
            QuerySolution* collscan = buildCollscanSoln(query, false, params);
            if (NULL != collscan) {
                SolutionCacheData* scd = new SolutionCacheData();
                scd->solnType = SolutionCacheData::COLLSCAN_SOLN;
                collscan->cacheData.reset(scd);
                out->push_back(collscan);
                QLOG() << "Planner: outputting a collscan:\n";
                QLOG() << collscan->toString() << endl;
            }
        }

        return Status::OK();
    }

}  // namespace mongo
