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

#include "mongo/db/query/query_planner_test_fixture.h"

#include <algorithm>

#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/util/log.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {

using unittest::assertGet;

const NamespaceString QueryPlannerTest::nss("test.collection");

void QueryPlannerTest::setUp() {
    opCtx = serviceContext.makeOperationContext();
    internalQueryPlannerEnableHashIntersection.store(true);
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("_id" << 1));
}

void QueryPlannerTest::clearState() {
    solns.clear();
    cq.reset();
    relaxBoundsCheck = false;
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, bool multikey) {
    params.indices.push_back({keyPattern,
                              IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                              multikey,
                              {},
                              {},
                              false,  // sparse
                              false,  // unique
                              IndexEntry::Identifier{"hari_king_of_the_stove"},
                              nullptr,  // filterExpr
                              BSONObj(),
                              nullptr,
                              nullptr});
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, bool multikey, bool sparse) {
    params.indices.push_back({keyPattern,
                              IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                              multikey,
                              {},
                              {},
                              sparse,
                              false,  // unique
                              IndexEntry::Identifier{"note_to_self_dont_break_build"},
                              nullptr,  // filterExpr
                              BSONObj(),
                              nullptr,
                              nullptr});
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, bool multikey, bool sparse, bool unique) {
    params.indices.push_back(
        {keyPattern,
         IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
         multikey,
         {},
         {},
         sparse,
         unique,
         IndexEntry::Identifier{"sql_query_walks_into_bar_and_says_can_i_join_you?"},
         nullptr,  // filterExpr
         BSONObj(),
         nullptr,
         nullptr});
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, BSONObj infoObj) {
    params.indices.push_back({keyPattern,
                              IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                              false,  // multikey
                              {},
                              {},
                              false,  // sparse
                              false,  // unique
                              IndexEntry::Identifier{"foo"},
                              nullptr,  // filterExpr
                              infoObj,
                              nullptr,
                              nullptr});
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, MatchExpression* filterExpr) {
    params.indices.push_back({keyPattern,
                              IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                              false,  // multikey
                              {},
                              {},
                              false,  // sparse
                              false,  // unique
                              IndexEntry::Identifier{"foo"},
                              filterExpr,
                              BSONObj(),
                              nullptr,
                              nullptr});
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, const MultikeyPaths& multikeyPaths) {
    invariant(multikeyPaths.size() == static_cast<size_t>(keyPattern.nFields()));

    const auto type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
    const bool multikey =
        std::any_of(multikeyPaths.cbegin(),
                    multikeyPaths.cend(),
                    [](const std::set<size_t>& components) { return !components.empty(); });
    const bool sparse = false;
    const bool unique = false;
    const char name[] = "my_index_with_path_level_multikey_info";
    const MatchExpression* filterExpr = nullptr;
    const BSONObj infoObj;
    IndexEntry entry(keyPattern,
                     type,
                     multikey,
                     {},
                     {},
                     sparse,
                     unique,
                     IndexEntry::Identifier{name},
                     filterExpr,
                     infoObj,
                     nullptr,
                     nullptr);
    entry.multikeyPaths = multikeyPaths;
    params.indices.push_back(entry);
}

void QueryPlannerTest::addIndex(BSONObj keyPattern, const CollatorInterface* collator) {
    const auto type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
    const bool sparse = false;
    const bool unique = false;
    const bool multikey = false;
    const char name[] = "my_index_with_collator";
    const MatchExpression* filterExpr = nullptr;
    const BSONObj infoObj;
    IndexEntry entry(keyPattern,
                     type,
                     multikey,
                     {},
                     {},
                     sparse,
                     unique,
                     IndexEntry::Identifier{name},
                     filterExpr,
                     infoObj,
                     nullptr,
                     nullptr);
    entry.collator = collator;
    params.indices.push_back(entry);
}

void QueryPlannerTest::addIndex(BSONObj keyPattern,
                                const CollatorInterface* collator,
                                StringData indexName) {
    const auto type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
    const bool sparse = false;
    const bool unique = false;
    const bool multikey = false;
    const auto name = indexName.toString();
    const MatchExpression* filterExpr = nullptr;
    const BSONObj infoObj;
    IndexEntry entry(keyPattern,
                     type,
                     multikey,
                     {},
                     {},
                     sparse,
                     unique,
                     IndexEntry::Identifier{name},
                     filterExpr,
                     infoObj,
                     nullptr,
                     nullptr);
    entry.collator = collator;
    params.indices.push_back(entry);
}

void QueryPlannerTest::addIndex(BSONObj keyPattern,
                                MatchExpression* filterExpr,
                                const CollatorInterface* collator) {
    const auto type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
    const bool sparse = false;
    const bool unique = false;
    const bool multikey = false;
    const char name[] = "my_partial_index_with_collator";
    const BSONObj infoObj;
    IndexEntry entry(keyPattern,
                     type,
                     multikey,
                     {},
                     {},
                     sparse,
                     unique,
                     IndexEntry::Identifier{name},
                     filterExpr,
                     infoObj,
                     nullptr,
                     nullptr);
    entry.collator = collator;
    params.indices.push_back(entry);
}

void QueryPlannerTest::addIndex(const IndexEntry& ie) {
    params.indices.push_back(ie);
}

void QueryPlannerTest::runQuery(BSONObj query) {
    runQuerySortProjSkipNToReturn(query, BSONObj(), BSONObj(), 0, 0);
}

void QueryPlannerTest::runQuerySortProj(const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& proj) {
    runQuerySortProjSkipNToReturn(query, sort, proj, 0, 0);
}

void QueryPlannerTest::runQuerySkipNToReturn(const BSONObj& query,
                                             long long skip,
                                             long long ntoreturn) {
    runQuerySortProjSkipNToReturn(query, BSONObj(), BSONObj(), skip, ntoreturn);
}

void QueryPlannerTest::runQueryHint(const BSONObj& query, const BSONObj& hint) {
    runQuerySortProjSkipNToReturnHint(query, BSONObj(), BSONObj(), 0, 0, hint);
}

void QueryPlannerTest::runQuerySortProjSkipNToReturn(const BSONObj& query,
                                                     const BSONObj& sort,
                                                     const BSONObj& proj,
                                                     long long skip,
                                                     long long ntoreturn) {
    runQuerySortProjSkipNToReturnHint(query, sort, proj, skip, ntoreturn, BSONObj());
}

void QueryPlannerTest::runQuerySortHint(const BSONObj& query,
                                        const BSONObj& sort,
                                        const BSONObj& hint) {
    runQuerySortProjSkipNToReturnHint(query, sort, BSONObj(), 0, 0, hint);
}

void QueryPlannerTest::runQueryHintMinMax(const BSONObj& query,
                                          const BSONObj& hint,
                                          const BSONObj& minObj,
                                          const BSONObj& maxObj) {
    runQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj);
}

void QueryPlannerTest::runQuerySortProjSkipNToReturnHint(const BSONObj& query,
                                                         const BSONObj& sort,
                                                         const BSONObj& proj,
                                                         long long skip,
                                                         long long ntoreturn,
                                                         const BSONObj& hint) {
    runQueryFull(query, sort, proj, skip, ntoreturn, hint, BSONObj(), BSONObj());
}

void QueryPlannerTest::runQueryFull(const BSONObj& query,
                                    const BSONObj& sort,
                                    const BSONObj& proj,
                                    long long skip,
                                    long long ntoreturn,
                                    const BSONObj& hint,
                                    const BSONObj& minObj,
                                    const BSONObj& maxObj) {
    clearState();

    auto qr = std::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    qr->setSort(sort);
    qr->setProj(proj);
    if (skip) {
        qr->setSkip(skip);
    }
    if (ntoreturn) {
        if (ntoreturn < 0) {
            ASSERT_NE(ntoreturn, std::numeric_limits<long long>::min());
            ntoreturn = -ntoreturn;
            qr->setWantMore(false);
        }
        qr->setNToReturn(ntoreturn);
    }
    qr->setHint(hint);
    qr->setMin(minObj);
    qr->setMax(maxObj);
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    cq = std::move(statusWithCQ.getValue());

    auto statusWithSolutions = QueryPlanner::plan(*cq, params);
    ASSERT_OK(statusWithSolutions.getStatus());
    solns = std::move(statusWithSolutions.getValue());
}

void QueryPlannerTest::runInvalidQuery(const BSONObj& query) {
    runInvalidQuerySortProjSkipNToReturn(query, BSONObj(), BSONObj(), 0, 0);
}

void QueryPlannerTest::runInvalidQuerySortProj(const BSONObj& query,
                                               const BSONObj& sort,
                                               const BSONObj& proj) {
    runInvalidQuerySortProjSkipNToReturn(query, sort, proj, 0, 0);
}

void QueryPlannerTest::runInvalidQuerySortProjSkipNToReturn(const BSONObj& query,
                                                            const BSONObj& sort,
                                                            const BSONObj& proj,
                                                            long long skip,
                                                            long long ntoreturn) {
    runInvalidQuerySortProjSkipNToReturnHint(query, sort, proj, skip, ntoreturn, BSONObj());
}

void QueryPlannerTest::runInvalidQueryHint(const BSONObj& query, const BSONObj& hint) {
    runInvalidQuerySortProjSkipNToReturnHint(query, BSONObj(), BSONObj(), 0, 0, hint);
}

void QueryPlannerTest::runInvalidQueryHintMinMax(const BSONObj& query,
                                                 const BSONObj& hint,
                                                 const BSONObj& minObj,
                                                 const BSONObj& maxObj) {
    runInvalidQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj);
}

void QueryPlannerTest::runInvalidQuerySortProjSkipNToReturnHint(const BSONObj& query,
                                                                const BSONObj& sort,
                                                                const BSONObj& proj,
                                                                long long skip,
                                                                long long ntoreturn,
                                                                const BSONObj& hint) {
    runInvalidQueryFull(query, sort, proj, skip, ntoreturn, hint, BSONObj(), BSONObj());
}

void QueryPlannerTest::runInvalidQueryFull(const BSONObj& query,
                                           const BSONObj& sort,
                                           const BSONObj& proj,
                                           long long skip,
                                           long long ntoreturn,
                                           const BSONObj& hint,
                                           const BSONObj& minObj,
                                           const BSONObj& maxObj) {
    clearState();

    auto qr = std::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    qr->setSort(sort);
    qr->setProj(proj);
    if (skip) {
        qr->setSkip(skip);
    }
    if (ntoreturn) {
        if (ntoreturn < 0) {
            ASSERT_NE(ntoreturn, std::numeric_limits<long long>::min());
            ntoreturn = -ntoreturn;
            qr->setWantMore(false);
        }
        qr->setNToReturn(ntoreturn);
    }
    qr->setHint(hint);
    qr->setMin(minObj);
    qr->setMax(maxObj);
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    cq = std::move(statusWithCQ.getValue());

    auto statusWithSolutions = QueryPlanner::plan(*cq, params);
    ASSERT_NOT_OK(statusWithSolutions.getStatus());
}

void QueryPlannerTest::runQueryAsCommand(const BSONObj& cmdObj) {
    clearState();

    invariant(nss.isValid());

    const bool isExplain = false;
    std::unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    cq = std::move(statusWithCQ.getValue());

    auto statusWithSolutions = QueryPlanner::plan(*cq, params);
    ASSERT_OK(statusWithSolutions.getStatus());
    solns = std::move(statusWithSolutions.getValue());
}

void QueryPlannerTest::runInvalidQueryAsCommand(const BSONObj& cmdObj) {
    clearState();

    invariant(nss.isValid());

    const bool isExplain = false;
    std::unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    cq = std::move(statusWithCQ.getValue());

    auto statusWithSolutions = QueryPlanner::plan(*cq, params);
    ASSERT_NOT_OK(statusWithSolutions.getStatus());
}

size_t QueryPlannerTest::getNumSolutions() const {
    return solns.size();
}

void QueryPlannerTest::dumpSolutions() const {
    str::stream ost;
    dumpSolutions(ost);
    log() << std::string(ost);
}

void QueryPlannerTest::dumpSolutions(str::stream& ost) const {
    for (auto&& soln : solns) {
        ost << soln->toString() << '\n';
    }
}

void QueryPlannerTest::assertNumSolutions(size_t expectSolutions) const {
    if (getNumSolutions() == expectSolutions) {
        return;
    }
    str::stream ss;
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
        if (QueryPlannerTestLib::solutionMatches(testSoln, root, relaxBoundsCheck)) {
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
    str::stream ss;
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
    str::stream ss;
    ss << "assertHasOneSolutionOf expected one matching solution"
       << " but got " << matches << " instead. all solutions generated: " << '\n';
    dumpSolutions(ss);
    FAIL(ss);
}

void QueryPlannerTest::assertHasOnlyCollscan() const {
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

std::unique_ptr<MatchExpression> QueryPlannerTest::parseMatchExpression(
    const BSONObj& obj, const CollatorInterface* collator) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(collator);
    StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
    if (!status.isOK()) {
        FAIL(str::stream() << "failed to parse query: " << obj.toString() << ". Reason: "
                           << status.getStatus().toString());
    }
    return std::move(status.getValue());
}

}  // namespace mongo
