/**
 *    Copyright (C) 2015 10gen Inc.
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

#include "mongo/db/query/query_planner_test_fixture.h"

#include <algorithm>

#include "mongo/db/namespace_string.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/util/log.h"

namespace mongo {

using unittest::assertGet;

const NamespaceString QueryPlannerTest::nss("test.collection");

void QueryPlannerTest::setUp() {
    internalQueryPlannerEnableHashIntersection = true;
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("_id" << 1));
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, bool multikey) {
    params.indices.push_back(IndexEntry(keyPattern,
                                        multikey,
                                        false,  // sparse
                                        false,  // unique
                                        "hari_king_of_the_stove",
                                        NULL,  // filterExpr
                                        BSONObj()));
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, bool multikey, bool sparse) {
    params.indices.push_back(IndexEntry(keyPattern,
                                        multikey,
                                        sparse,
                                        false,  // unique
                                        "note_to_self_dont_break_build",
                                        NULL,  // filterExpr
                                        BSONObj()));
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, bool multikey, bool sparse, bool unique) {
    params.indices.push_back(IndexEntry(keyPattern,
                                        multikey,
                                        sparse,
                                        unique,
                                        "sql_query_walks_into_bar_and_says_can_i_join_you?",
                                        NULL,  // filterExpr
                                        BSONObj()));
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, BSONObj infoObj) {
    params.indices.push_back(IndexEntry(keyPattern,
                                        false,  // multikey
                                        false,  // sparse
                                        false,  // unique
                                        "foo",
                                        NULL,  // filterExpr
                                        infoObj));
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, MatchExpression* filterExpr) {
    params.indices.push_back(IndexEntry(keyPattern,
                                        false,  // multikey
                                        false,  // sparse
                                        false,  // unique
                                        "foo",
                                        filterExpr,
                                        BSONObj()));
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, MultikeyPaths multikeyPaths) {
    invariant(multikeyPaths.size() == static_cast<size_t>(keyPattern.nFields()));

    const bool multikey =
        std::any_of(multikeyPaths.cbegin(),
                    multikeyPaths.cend(),
                    [](const std::set<size_t>& components) { return !components.empty(); });
    const bool sparse = false;
    const bool unique = false;
    const char name[] = "my_index_with_path_level_multikey_info";
    const MatchExpression* filterExpr = nullptr;
    const BSONObj infoObj;
    IndexEntry entry(keyPattern, multikey, sparse, unique, name, filterExpr, infoObj);
    entry.multikeyPaths = multikeyPaths;
    params.indices.push_back(entry);
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, CollatorInterface* collator) {
    const bool sparse = false;
    const bool unique = false;
    const bool multikey = false;
    const char name[] = "my_index_with_collator";
    const MatchExpression* filterExpr = nullptr;
    const BSONObj infoObj;
    IndexEntry entry(keyPattern, multikey, sparse, unique, name, filterExpr, infoObj);
    entry.collator = collator;
    params.indices.push_back(entry);
}

void QueryPlannerTest::runQuery(BSONObj query) {
    runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), 0, 0);
}

void QueryPlannerTest::runQuerySortProj(const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& proj) {
    runQuerySortProjSkipLimit(query, sort, proj, 0, 0);
}

void QueryPlannerTest::runQuerySkipLimit(const BSONObj& query, long long skip, long long limit) {
    runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), skip, limit);
}

void QueryPlannerTest::runQueryHint(const BSONObj& query, const BSONObj& hint) {
    runQuerySortProjSkipLimitHint(query, BSONObj(), BSONObj(), 0, 0, hint);
}

void QueryPlannerTest::runQuerySortProjSkipLimit(const BSONObj& query,
                                                 const BSONObj& sort,
                                                 const BSONObj& proj,
                                                 long long skip,
                                                 long long limit) {
    runQuerySortProjSkipLimitHint(query, sort, proj, skip, limit, BSONObj());
}

void QueryPlannerTest::runQuerySortHint(const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& hint) {
    runQuerySortProjSkipLimitHint(query, sort, BSONObj(), 0, 0, hint);
}

void QueryPlannerTest::runQueryHintMinMax(const BSONObj& query,
                                          const BSONObj& hint,
                                          const BSONObj& minObj,
                                          const BSONObj& maxObj) {
    runQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj, false);
}

void QueryPlannerTest::runQuerySortProjSkipLimitHint(const BSONObj& query,
                                                     const BSONObj& sort,
                                                     const BSONObj& proj,
                                                     long long skip,
                                                     long long limit,
                                                     const BSONObj& hint) {
    runQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj(), false);
}

void QueryPlannerTest::runQuerySnapshot(const BSONObj& query) {
    runQueryFull(query, BSONObj(), BSONObj(), 0, 0, BSONObj(), BSONObj(), BSONObj(), true);
}

void QueryPlannerTest::runQueryFull(const BSONObj& query,
                                    const BSONObj& sort,
                                    const BSONObj& proj,
                                    long long skip,
                                    long long limit,
                                    const BSONObj& hint,
                                    const BSONObj& minObj,
                                    const BSONObj& maxObj,
                                    bool snapshot) {
    // Clean up any previous state from a call to runQueryFull
    solns.clear();
    cq.reset();

    auto statusWithCQ = CanonicalQuery::canonicalize(nss,
                                                     query,
                                                     sort,
                                                     proj,
                                                     skip,
                                                     limit,
                                                     hint,
                                                     minObj,
                                                     maxObj,
                                                     snapshot,
                                                     false,  // explain
                                                     ExtensionsCallbackNoop());
    ASSERT_OK(statusWithCQ.getStatus());
    cq = std::move(statusWithCQ.getValue());

    ASSERT_OK(QueryPlanner::plan(*cq, params, &solns.mutableVector()));
}

void QueryPlannerTest::runInvalidQuery(const BSONObj& query) {
    runInvalidQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), 0, 0);
}

void QueryPlannerTest::runInvalidQuerySortProj(const BSONObj& query,
                                               const BSONObj& sort,
                                               const BSONObj& proj) {
    runInvalidQuerySortProjSkipLimit(query, sort, proj, 0, 0);
}

void QueryPlannerTest::runInvalidQuerySortProjSkipLimit(const BSONObj& query,
                                                        const BSONObj& sort,
                                                        const BSONObj& proj,
                                                        long long skip,
                                                        long long limit) {
    runInvalidQuerySortProjSkipLimitHint(query, sort, proj, skip, limit, BSONObj());
}

void QueryPlannerTest::runInvalidQueryHint(const BSONObj& query, const BSONObj& hint) {
    runInvalidQuerySortProjSkipLimitHint(query, BSONObj(), BSONObj(), 0, 0, hint);
}

void QueryPlannerTest::runInvalidQueryHintMinMax(const BSONObj& query,
                                                 const BSONObj& hint,
                                                 const BSONObj& minObj,
                                                 const BSONObj& maxObj) {
    runInvalidQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj, false);
}

void QueryPlannerTest::runInvalidQuerySortProjSkipLimitHint(const BSONObj& query,
                                                            const BSONObj& sort,
                                                            const BSONObj& proj,
                                                            long long skip,
                                                            long long limit,
                                                            const BSONObj& hint) {
    runInvalidQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj(), false);
}

void QueryPlannerTest::runInvalidQueryFull(const BSONObj& query,
                                           const BSONObj& sort,
                                           const BSONObj& proj,
                                           long long skip,
                                           long long limit,
                                           const BSONObj& hint,
                                           const BSONObj& minObj,
                                           const BSONObj& maxObj,
                                           bool snapshot) {
    solns.clear();
    cq.reset();

    auto statusWithCQ = CanonicalQuery::canonicalize(nss,
                                                     query,
                                                     sort,
                                                     proj,
                                                     skip,
                                                     limit,
                                                     hint,
                                                     minObj,
                                                     maxObj,
                                                     snapshot,
                                                     false,  // explain
                                                     ExtensionsCallbackNoop());
    ASSERT_OK(statusWithCQ.getStatus());
    cq = std::move(statusWithCQ.getValue());

    Status s = QueryPlanner::plan(*cq, params, &solns.mutableVector());
    ASSERT_NOT_OK(s);
}

void QueryPlannerTest::runQueryAsCommand(const BSONObj& cmdObj) {
    solns.clear();
    cq.reset();

    invariant(nss.isValid());

    const bool isExplain = false;
    std::unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    auto statusWithCQ = CanonicalQuery::canonicalize(lpq.release(), ExtensionsCallbackNoop());
    ASSERT_OK(statusWithCQ.getStatus());
    cq = std::move(statusWithCQ.getValue());

    Status s = QueryPlanner::plan(*cq, params, &solns.mutableVector());
    ASSERT_OK(s);
}

size_t QueryPlannerTest::getNumSolutions() const {
    return solns.size();
}

void QueryPlannerTest::dumpSolutions() const {
    mongoutils::str::stream ost;
    dumpSolutions(ost);
    log() << std::string(ost);
}

void QueryPlannerTest::dumpSolutions(mongoutils::str::stream& ost) const {
    for (auto&& soln : solns) {
        ost << soln->toString() << '\n';
    }
}

void QueryPlannerTest::assertNumSolutions(size_t expectSolutions) const {
    if (getNumSolutions() == expectSolutions) {
        return;
    }
    mongoutils::str::stream ss;
    ss << "expected " << expectSolutions << " solutions but got " << getNumSolutions()
       << " instead. solutions generated: " << '\n';
    dumpSolutions(ss);
    FAIL(ss);
}

size_t QueryPlannerTest::numSolutionMatches(const std::string& solnJson) const {
    BSONObj testSoln = fromjson(solnJson);
    size_t matches = 0;
    for (auto&& soln : solns) {
        QuerySolutionNode* root = soln->root.get();
        if (QueryPlannerTestLib::solutionMatches(testSoln, root)) {
            ++matches;
        }
    }
    return matches;
}

void QueryPlannerTest::assertSolutionExists(const std::string& solnJson, size_t numMatches) const {
    size_t matches = numSolutionMatches(solnJson);
    if (numMatches == matches) {
        return;
    }
    mongoutils::str::stream ss;
    ss << "expected " << numMatches << " matches for solution " << solnJson << " but got "
       << matches << " instead. all solutions generated: " << '\n';
    dumpSolutions(ss);
    FAIL(ss);
}

void QueryPlannerTest::assertHasOneSolutionOf(const std::vector<std::string>& solnStrs) const {
    size_t matches = 0;
    for (std::vector<std::string>::const_iterator it = solnStrs.begin(); it != solnStrs.end();
         ++it) {
        if (1U == numSolutionMatches(*it)) {
            ++matches;
        }
    }
    if (1U == matches) {
        return;
    }
    mongoutils::str::stream ss;
    ss << "assertHasOneSolutionOf expected one matching solution"
       << " but got " << matches << " instead. all solutions generated: " << '\n';
    dumpSolutions(ss);
    FAIL(ss);
}

std::unique_ptr<MatchExpression> QueryPlannerTest::parseMatchExpression(const BSONObj& obj) {
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj, ExtensionsCallbackDisallowExtensions());
    if (!status.isOK()) {
        FAIL(str::stream() << "failed to parse query: " << obj.toString()
                           << ". Reason: " << status.getStatus().toString());
    }
    return std::move(status.getValue());
}

}  // namespace mongo
