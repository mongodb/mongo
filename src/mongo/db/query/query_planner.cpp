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

    // Copied verbatim from queryutil.cpp.
    static bool isSimpleIdQuery(const BSONObj& query) {
        BSONObjIterator it(query);

        if (!it.more()) {
            return false;
        }

        BSONElement elt = it.next();

        if (it.more()) {
            return false;
        }

        if (strcmp("_id", elt.fieldName()) != 0) {
            return false;
        }

        // e.g. not something like { _id : { $gt : ...
        if (elt.isSimpleType()) {
            return true;
        }

        if (elt.type() == Object) {
            return elt.Obj().firstElementFieldName()[0] != '$';
        }

        return false;
    }

    string optionString(size_t options) {
        stringstream ss;

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
        return ss.str();
    }

    static BSONObj getKeyFromQuery(const BSONObj& keyPattern, const BSONObj& query) {
        return query.extractFieldsUnDotted(keyPattern);
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

    // static
    void QueryPlanner::plan(const CanonicalQuery& query,
                            const QueryPlannerParams& params,
                            vector<QuerySolution*>* out) {
        QLOG() << "=============================\n"
               << "Beginning planning, options = " << optionString(params.options) << endl
               << "Canonical query:\n" << query.toString() << endl
               << "============================="
               << endl;

        // The shortcut formerly known as IDHACK.  See if it's a simple _id query.  If so we might
        // just make an ixscan over the _id index and bypass the rest of planning entirely.
        if (!query.getParsed().isExplain() && !query.getParsed().showDiskLoc()
            && isSimpleIdQuery(query.getParsed().getFilter())
            && !query.getParsed().hasOption(QueryOption_CursorTailable)) {

            // See if we can find an _id index.
            for (size_t i = 0; i < params.indices.size(); ++i) {
                if (isIdIndex(params.indices[i].keyPattern)) {
                    const IndexEntry& index = params.indices[i];
                    QLOG() << "IDHACK using index " << index.toString() << endl;

                    // If so, we make a simple scan to find the doc.
                    IndexScanNode* isn = new IndexScanNode();
                    isn->indexKeyPattern = index.keyPattern;
                    isn->indexIsMultiKey = index.multikey;
                    isn->direction = 1;
                    isn->bounds.isSimpleRange = true;
                    BSONObj key = getKeyFromQuery(index.keyPattern, query.getParsed().getFilter());
                    isn->bounds.startKey = isn->bounds.endKey = key;
                    isn->bounds.endKeyInclusive = true;
                    isn->computeProperties();

                    QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, isn);

                    if (NULL != soln) {
                        out->push_back(soln);
                        QLOG() << "IDHACK solution is:\n" << (*out)[0]->toString() << endl;
                        // And that's it.
                        return;
                    }
                }
            }
        }

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
            return;
        }

        // The hint can be $natural: 1.  If this happens, output a collscan.  It's a weird way of
        // saying "table scan for two, please."
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                QLOG() << "forcing a table scan due to hinted $natural\n";
                if (canTableScan) {
                    QuerySolution* soln = buildCollscanSoln(query, false, params);
                    if (NULL != soln) {
                        out->push_back(soln);
                    }
                }
                return;
            }
        }

        // NOR and NOT we can't handle well with indices.  If we see them here, they weren't
        // rewritten to remove the negation.  Just output a collscan for those.
        if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::NOT)
            || QueryPlannerCommon::hasNode(query.root(), MatchExpression::NOR)) {

            // If there's a near predicate, we can't handle this.
            // TODO: Should canonicalized query detect this?
            if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)) {
                warning() << "Can't handle NOT/NOR with GEO_NEAR";
                return;
            }
            QLOG() << "NOT/NOR in plan, just outtping a collscan\n";
            if (canTableScan) {
                QuerySolution* soln = buildCollscanSoln(query, false, params);
                if (NULL != soln) {
                    out->push_back(soln);
                }
            }
            return;
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
        BSONObj hintIndex = query.getParsed().getHint();

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

        if (!hintIndex.isEmpty()) {
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
                // This is supposed to be an error.
                warning() << "Can't find hint for " << hintIndex.toString();
                return;
            }
        }
        else {
            QLOG() << "Finding relevant indices\n";
            QueryPlannerIXSelect::findRelevantIndices(fields, params.indices, &relevantIndices);
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
                return;
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
                return;
            }
        }

        // Likewise, if there is a TEXT it must have an index it can use directly.
        MatchExpression* textNode;
        if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT, &textNode)) {
            RelevantTag* tag = static_cast<RelevantTag*>(textNode->getTag());
            if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
                return;
            }
        }

        // If we have any relevant indices, we try to create indexed plans.
        if (0 < relevantIndices.size()) {
            // The enumerator spits out trees tagged with IndexTag(s).
            PlanEnumerator isp(query.root(), &relevantIndices);
            isp.init();

            MatchExpression* rawTree;
            while (isp.getNext(&rawTree)) {
                QLOG() << "about to build solntree from tagged tree:\n" << rawTree->toString()
                       << endl;

                // This can fail if enumeration makes a mistake.
                QuerySolutionNode* solnRoot =
                    QueryPlannerAccess::buildIndexedDataAccess(query, rawTree, false, relevantIndices);

                if (NULL == solnRoot) { continue; }

                QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(query, params, solnRoot);
                if (NULL != soln) {
                    QLOG() << "Planner: adding solution:\n" << soln->toString() << endl;
                    out->push_back(soln);
                }
            }
        }

        QLOG() << "Planner: outputted " << out->size() << " indexed solutions.\n";

        // An index was hinted.  If there are any solutions, they use the hinted index.  If not, we
        // scan the entire index to provide results and output that as our plan.  This is the
        // desired behavior when an index is hinted that is not relevant to the query.
        if (!hintIndex.isEmpty() && (0 == out->size())) {
            QuerySolution* soln = buildWholeIXSoln(params.indices[hintIndexNumber], query, params);
            if (NULL != soln) {
                QLOG() << "Planner: outputting soln that uses hinted index as scan." << endl;
                out->push_back(soln);
            }
            return;
        }

        // If a sort order is requested, there may be an index that provides it, even if that
        // index is not over any predicates in the query.
        //
        // XXX XXX: Can we do this even if the index is sparse?  Might we miss things?
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
                    const BSONObj& kp = index.keyPattern;
                    if (providesSort(query, kp)) {
                        QLOG() << "Planner: outputting soln that uses index to provide sort."
                               << endl;
                        QuerySolution* soln = buildWholeIXSoln(params.indices[i], query, params);
                        if (NULL != soln) {
                            out->push_back(soln);
                            break;
                        }
                    }
                    if (providesSort(query, QueryPlannerCommon::reverseSortObj(kp))) {
                        QLOG() << "Planner: outputting soln that uses (reverse) index "
                               << "to provide sort." << endl;
                        QuerySolution* soln = buildWholeIXSoln(params.indices[i], query, params, -1);
                        if (NULL != soln) {
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
             && ((params.options & QueryPlannerParams::INCLUDE_COLLSCAN) || (0 == out->size() && canTableScan)))
        {
            QuerySolution* collscan = buildCollscanSoln(query, false, params);
            if (NULL != collscan) {
                out->push_back(collscan);
                QLOG() << "Planner: outputting a collscan:\n";
                QLOG() << collscan->toString() << endl;
            }
        }
    }

}  // namespace mongo
