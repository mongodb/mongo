/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <string>
#include <vector>

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

namespace mongo::optimizer {
namespace {
using namespace unit_test_abt_literals;

ABT optimizedQueryPlan(const std::string& query,
                       const opt::unordered_map<std::string, IndexDefinition>& indexes) {
    auto prefixId = PrefixId::createForTests();
    std::string scanDefName = "coll";
    Metadata metadata = {{{scanDefName, createScanDef({}, indexes)}}};
    ABT translated = translatePipeline(
        metadata, "[{$match: " + query + "}]", prefixId.getNextId("scan"), scanDefName, prefixId);

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase,
                                          OptPhase::MemoExplorationPhase,
                                          OptPhase::MemoImplementationPhase},
                                         prefixId,
                                         metadata,
                                         boost::none /*costModel*/,
                                         DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    phaseManager.getHints()._disableScan = true;
    phaseManager.optimize(optimized);
    return optimized;
}

class IntervalIntersection : public LockerNoopServiceContextTest {};

TEST_F(IntervalIntersection, SingleFieldIntersection) {
    opt::unordered_map<std::string, IndexDefinition> testIndex = {
        {"index1", makeIndexDefinition("a0", CollationOp::Ascending, /*Not multikey*/ false)}};

    const std::string q1Text = "{a0: {$gt:14, $lt:21}}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: "
        "{(Const [14], Const [21])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q1Text, testIndex));

    const std::string q2Text = "{$and: [{a0: {$gt:14}}, {a0: {$lt: 21}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: "
        "{(Const [14], Const [21])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q2Text, testIndex));

    const std::string q3Text =
        "{$or: [{$and: [{a0: {$gt:9, $lt:999}}, {a0: {$gt: 0, $lt: 12}}]}, {$and: [{a0: {$gt:40, "
        "$lt:997}}, {a0: {$gt:0, $lt: 44}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [rid_0]\n"
        "|   aggregations: \n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [rid_0]\n"
        "|   |           Source []\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: {(Co"
        "nst [40], Const [44])}]\n"
        "|       BindBlock:\n"
        "|           [rid_0]\n"
        "|               Source []\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: {(Const "
        "[9], Const [12])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q3Text, testIndex));

    // Contradiction: empty interval.
    const std::string q4Text = "{$and: [{a0: {$gt:20}}, {a0: {$lt: 20}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q4Text, testIndex));

    // Contradiction: one conjunct non-empty, one conjunct empty.
    const std::string q5Text =
        "{$or: [{$and: [{a0: {$gt:9}}, {a0: {$lt: 12}}]}, {$and: [{a0: {$gt:44}}, {a0: {$lt: "
        "40}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, coll]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: coll, indexDefName: index1, interval: "
        "{(Const [9], Const [12])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimizedQueryPlan(q5Text, testIndex));

    // Contradiction: both conjuncts empty, whole disjunct empty.
    const std::string q6Text =
        "{$or: [{$and: [{a0: {$gt:15}}, {a0: {$lt: 10}}]}, {$and: [{a0: {$gt:44}}, {a0: {$lt: "
        "40}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q6Text, testIndex));

    // Contradiction.
    const std::string q7Text =
        "{$or: [{$and: [{a0: {$gt:12}}, {a0: {$lt: 12}}]}, {$and: [{a0: {$gte:42}}, {a0: {$lt: "
        "42}}]}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q7Text, testIndex));
}

TEST_F(IntervalIntersection, MultiFieldIntersection) {
    std::vector<TestIndexField> indexFields{{"a0", CollationOp::Ascending, false},
                                            {"b0", CollationOp::Ascending, false}};

    opt::unordered_map<std::string, IndexDefinition> testIndex = {
        {"index1", makeCompositeIndexDefinition(indexFields, false /*isMultiKey*/)}};

    // Note those are queries below are contradictions.

    const std::string q1Text =
        "{$and: [{a0: {$gt: 11}}, {a0: {$lt: 14}}, {b0: {$gt: 21}}, {b0: {$lt: 12}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q1Text, testIndex));

    const std::string q2Text =
        "{$and: [{a0: {$gt: 14}}, {a0: {$lt: 11}}, {b0: {$gt: 12}}, {b0: {$lt: 21}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q2Text, testIndex));

    const std::string q3Text =
        "{$and: [{a0: {$gt: 14}}, {a0: {$lt: 11}}, {b0: {$gt: 21}}, {b0: {$lt: 12}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q3Text, testIndex));

    const std::string q4Text = "{$and: [{a0: 42}, {b0: {$gt: 21}}, {b0: {$lt: 12}}]}";
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [scan_0]\n"
        "|           Const [Nothing]\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 0\n"
        "|       skip: 0\n"
        "CoScan []\n",
        optimizedQueryPlan(q4Text, testIndex));
}

TEST_F(IntervalIntersection, VariableIntervals1) {
    auto interval = _disj(
        _conj(_interval(_incl("v1"_var), _plusInf()), _interval(_excl("v2"_var), _plusInf())));

    auto result = intersectDNFIntervals(interval, ConstEval::constFold);
    ASSERT_TRUE(result);

    // (max(v1, v2), +inf) U [v2 >= v1 ? MaxKey : v1, max(v1, v2)]
    ASSERT_INTERVAL(
        "{\n"
        "    {\n"
        "        {[If [] BinaryOp [And] BinaryOp [And] BinaryOp [Or] BinaryOp [Or] BinaryOp [And] "
        "BinaryOp [Lt] Variable [v2] Variable [v1] Const [true] BinaryOp [And] BinaryOp [Lt] "
        "Variable [v2] Const [maxKey] Const [true] BinaryOp [Or] BinaryOp [And] BinaryOp [Lt] "
        "Variable [v1] Variable [v2] BinaryOp [Lt] Variable [v2] Const [maxKey] Const [true] "
        "BinaryOp [Lt] Variable [v2] Const [maxKey] BinaryOp [Gt] Variable [v1] Variable [v2] "
        "Variable [v1] Const [maxKey], Variable [v1]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {>If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable [v2]}\n"
        "    }\n"
        "}\n",
        *result);

    // Make sure repeated intersection does not change the result.
    auto result1 = intersectDNFIntervals(*result, ConstEval::constFold);
    ASSERT_TRUE(result1);
    ASSERT_TRUE(*result == *result1);
}

TEST_F(IntervalIntersection, VariableIntervals2) {
    auto interval = _disj(_conj(_interval(_incl("v1"_var), _incl("v3"_var)),
                                _interval(_incl("v2"_var), _incl("v4"_var))));

    auto result = intersectDNFIntervals(interval, ConstEval::constFold);
    ASSERT_TRUE(result);

    // [v1, v3] ^ [v2, v4] -> [max(v1, v2), min(v3, v4)]
    ASSERT_INTERVAL(
        "{\n"
        "    {\n"
        "        {[If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable "
        "[v2], If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4]]}\n"
        "    }\n"
        "}\n",
        *result);

    // Make sure repeated intersection does not change the result.
    auto result1 = intersectDNFIntervals(*result, ConstEval::constFold);
    ASSERT_TRUE(result1);
    ASSERT_TRUE(*result == *result1);
}

TEST_F(IntervalIntersection, VariableIntervals3) {
    auto interval = _disj(_conj(_interval(_excl("v1"_var), _incl("v3"_var)),
                                _interval(_incl("v2"_var), _incl("v4"_var))));

    auto result = intersectDNFIntervals(interval, ConstEval::constFold);
    ASSERT_TRUE(result);

    ASSERT_INTERVAL(
        "{\n"
        "    {\n"
        "        {[If [] BinaryOp [And] BinaryOp [And] BinaryOp [Or] BinaryOp [Or] BinaryOp [And] "
        "BinaryOp [Lt] Variable [v2] Variable [v1] BinaryOp [Lt] Variable [v1] Variable [v4] "
        "BinaryOp [And] BinaryOp [Lte] Variable [v2] Variable [v3] BinaryOp [Lte] Variable [v3] "
        "Variable [v4] BinaryOp [Or] BinaryOp [And] BinaryOp [Lt] Variable [v1] Variable [v2] "
        "BinaryOp [Lte] Variable [v2] Variable [v3] BinaryOp [And] BinaryOp [Lt] Variable [v1] "
        "Variable [v4] BinaryOp [Lte] Variable [v4] Variable [v3] BinaryOp [And] BinaryOp [Lt] "
        "Variable [v1] Variable [v3] BinaryOp [Lte] Variable [v2] Variable [v4] BinaryOp [Gt] "
        "Variable [v2] Variable [v1] Variable [v2] Const [maxKey], Variable [v2]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {(If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable [v2], "
        "If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4]]}\n"
        "    }\n"
        "}\n",
        *result);

    // Make sure repeated intersection does not change the result.
    auto result1 = intersectDNFIntervals(*result, ConstEval::constFold);
    ASSERT_TRUE(result1);
    ASSERT_TRUE(*result == *result1);
}

TEST_F(IntervalIntersection, VariableIntervals4) {
    auto interval = _disj(_conj(_interval(_excl("v1"_var), _incl("v3"_var)),
                                _interval(_incl("v2"_var), _excl("v4"_var))));

    auto result = intersectDNFIntervals(interval, ConstEval::constFold);
    ASSERT_TRUE(result);

    ASSERT_INTERVAL(
        "{\n"
        "    {\n"
        "        {[If [] BinaryOp [And] BinaryOp [And] BinaryOp [Or] BinaryOp [Or] BinaryOp [And] "
        "BinaryOp [Lt] Variable [v2] Variable [v1] BinaryOp [Lt] Variable [v1] Variable [v4] "
        "BinaryOp [And] BinaryOp [Lte] Variable [v2] Variable [v3] BinaryOp [Lt] Variable [v3] "
        "Variable [v4] BinaryOp [Or] BinaryOp [And] BinaryOp [Lt] Variable [v1] Variable [v2] "
        "BinaryOp [Lte] Variable [v2] Variable [v3] BinaryOp [And] BinaryOp [Lt] Variable [v1] "
        "Variable [v4] BinaryOp [Lt] Variable [v4] Variable [v3] BinaryOp [And] BinaryOp [Lt] "
        "Variable [v1] Variable [v3] BinaryOp [Lt] Variable [v2] Variable [v4] BinaryOp [Gt] "
        "Variable [v2] Variable [v1] Variable [v2] Const [maxKey], Variable [v2]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Variable [v3], If [] BinaryOp [And] BinaryOp [And] BinaryOp [Or] BinaryOp [Or] "
        "BinaryOp [And] BinaryOp [Lt] Variable [v2] Variable [v1] BinaryOp [Lt] Variable [v1] "
        "Variable [v4] BinaryOp [And] BinaryOp [Lte] Variable [v2] Variable [v3] BinaryOp [Lt] "
        "Variable [v3] Variable [v4] BinaryOp [Or] BinaryOp [And] BinaryOp [Lt] Variable [v1] "
        "Variable [v2] BinaryOp [Lte] Variable [v2] Variable [v3] BinaryOp [And] BinaryOp [Lt] "
        "Variable [v1] Variable [v4] BinaryOp [Lt] Variable [v4] Variable [v3] BinaryOp [And] "
        "BinaryOp [Lt] Variable [v1] Variable [v3] BinaryOp [Lt] Variable [v2] Variable [v4] "
        "BinaryOp [Lt] Variable [v3] Variable [v4] Variable [v3] Const [minKey]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {(If [] BinaryOp [Gte] Variable [v1] Variable [v2] Variable [v1] Variable [v2], "
        "If [] BinaryOp [Lte] Variable [v3] Variable [v4] Variable [v3] Variable [v4])}\n"
        "    }\n"
        "}\n",
        *result);

    // Make sure repeated intersection does not change the result.
    auto result1 = intersectDNFIntervals(*result, ConstEval::constFold);
    ASSERT_TRUE(result1);
    ASSERT_TRUE(*result == *result1);
}

/*
 * Bitset with extra flags to indicate whether MinKey and MaxKey are included.
 * The first two bits are MinKey and MaxKey, the rest represent integers [0, N).
 */
template <int N>
class ExtendedBitset {
public:
    ExtendedBitset() {}

    void set(const int i, const bool b) {
        invariant(i >= 0 && i < N);
        _b.set(i + 2, b);
    }

    static ExtendedBitset<N> minKey() {
        ExtendedBitset<N> b;
        b._b.set(0);
        return b;
    }

    static ExtendedBitset<N> maxKey() {
        ExtendedBitset<N> b;
        b._b.set(1);
        return b;
    }

    ExtendedBitset& operator&=(const ExtendedBitset& rhs) {
        _b &= rhs._b;
        return *this;
    }

    ExtendedBitset& operator|=(const ExtendedBitset& rhs) {
        _b |= rhs._b;
        return *this;
    }

    bool operator==(const ExtendedBitset& rhs) const {
        return _b == rhs._b;
    }

    bool operator!=(const ExtendedBitset& rhs) const {
        return !(*this == rhs);
    }

private:
    std::bitset<N + 2> _b;
};

/*
 * Calculates the extended bitset of a given interval in any form (not just DNF).
 */
template <int N>
class IntervalInclusionTransport {
public:
    using ResultType = ExtendedBitset<N>;

    ResultType transport(const IntervalReqExpr::Atom& node) {
        const auto& expr = node.getExpr();
        const auto& lb = expr.getLowBound();
        const auto& hb = expr.getHighBound();

        ExtendedBitset<N> result;
        if (lb.getBound() == Constant::maxKey() || hb.getBound() == Constant::minKey()) {
            return result;
        }

        int lbInt = 0;
        if (lb.getBound() == Constant::minKey()) {
            result |= ExtendedBitset<N>::minKey();
        } else {
            lbInt = lb.getBound().cast<Constant>()->getValueInt32() + (lb.isInclusive() ? 0 : 1);
        }

        int hbInt = N;
        if (hb.getBound() == Constant::maxKey()) {
            result |= ExtendedBitset<N>::maxKey();
        } else {
            hbInt = hb.getBound().cast<Constant>()->getValueInt32() + (hb.isInclusive() ? 1 : 0);
        }

        for (int v = lbInt; v < hbInt; v++) {
            result.set(v, true);
        }

        return result;
    }

    ResultType transport(const IntervalReqExpr::Conjunction& node,
                         std::vector<ResultType> childResults) {
        for (size_t index = 1; index < childResults.size(); index++) {
            childResults.front() &= childResults.at(index);
        }
        return childResults.front();
    }

    ResultType transport(const IntervalReqExpr::Disjunction& node,
                         std::vector<ResultType> childResults) {
        for (size_t index = 1; index < childResults.size(); index++) {
            childResults.front() |= childResults.at(index);
        }
        return childResults.front();
    }

    ResultType computeInclusion(const IntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }
};

/*
 * Replaces variables with their value in the given varMap.
 */
class EvalVariables {
public:
    EvalVariables(ProjectionNameMap<ABT> varMap) : _varMap(std::move(varMap)) {}

    void transport(ABT& n, const Variable& node) {
        const auto it = _varMap.find(ProjectionName(node.name().value()));
        if (it != _varMap.end()) {
            n = it->second;
        }
    }

    template <typename T, typename... Ts>
    void transport(ABT& /*n*/, const T& /*node*/, Ts&&...) {
        invariant((std::is_base_of_v<If, T> || std::is_base_of_v<BinaryOp, T> ||
                   std::is_base_of_v<UnaryOp, T> || std::is_base_of_v<Constant, T> ||
                   std::is_base_of_v<Variable, T>));
    }

    void evalVars(ABT& n) {
        algebra::transport<true>(n, *this);
        ConstEval::constFold(n);
        invariant(n.is<Constant>());
    }

    void replaceVarsInInterval(IntervalReqExpr::Node& interval) {
        for (auto& disjunct : interval.cast<IntervalReqExpr::Disjunction>()->nodes()) {
            for (auto& conjunct : disjunct.cast<IntervalReqExpr::Conjunction>()->nodes()) {
                auto& interval = conjunct.cast<IntervalReqExpr::Atom>()->getExpr();
                ABT lowBound = interval.getLowBound().getBound();
                ABT highBound = interval.getHighBound().getBound();
                evalVars(lowBound);
                evalVars(highBound);
                interval = {{interval.getLowBound().isInclusive(), std::move(lowBound)},
                            {interval.getHighBound().isInclusive(), std::move(highBound)}};
            }
        }
    }

private:
    ProjectionNameMap<ABT> _varMap;
};

template <int V>
int decode(int& permutation) {
    const int result = permutation % V;
    permutation /= V;
    return result;
}

template <int N>
bool compareIntervals(const IntervalReqExpr::Node& original,
                      const IntervalReqExpr::Node& simplified) {
    IntervalInclusionTransport<N> transport;
    return transport.computeInclusion(original) == transport.computeInclusion(simplified);
}

void constFoldBounds(IntervalReqExpr::Node& node) {
    for (auto& disjunct : node.cast<IntervalReqExpr::Disjunction>()->nodes()) {
        for (auto& conjunct : disjunct.cast<IntervalReqExpr::Conjunction>()->nodes()) {
            auto& interval = conjunct.cast<IntervalReqExpr::Atom>()->getExpr();
            ABT lowABT = interval.getLowBound().getBound();
            ABT highABT = interval.getHighBound().getBound();
            ConstEval::constFold(lowABT);
            ConstEval::constFold(highABT);
            interval = IntervalRequirement{
                BoundRequirement{interval.getLowBound().isInclusive(), std::move(lowABT)},
                BoundRequirement{interval.getHighBound().isInclusive(), std::move(highABT)},
            };
        }
    }
}

/*
 * Create two random intervals composed of constants and test intersection/union on them.
 */
template <int N>
void testIntervalPermutation(int permutation) {
    const bool low1Inc = decode<2>(permutation);
    const int low1 = decode<N>(permutation);
    const bool high1Inc = decode<2>(permutation);
    const int high1 = decode<N>(permutation);
    const bool low2Inc = decode<2>(permutation);
    const int low2 = decode<N>(permutation);
    const bool high2Inc = decode<2>(permutation);
    const int high2 = decode<N>(permutation);
    const bool useRealConstFold = decode<2>(permutation);

    // This function can be passed as a substitute for the real constant folding function, to test
    // that our simplification methods work when we cannot constant fold anything.
    const auto noOpConstFold = [](ABT& n) {
        // No-op.
    };

    // Test intersection.
    {
        auto original = _disj(
            _conj(_interval({low1Inc, Constant::int32(low1)}, {high1Inc, Constant::int32(high1)}),
                  _interval({low2Inc, Constant::int32(low2)}, {high2Inc, Constant::int32(high2)})));
        auto simplified = intersectDNFIntervals(
            original, useRealConstFold ? ConstEval::constFold : noOpConstFold);

        if (simplified) {
            if (useRealConstFold) {
                // Since we are testing with constants, we should have at most one interval as long
                // as we use real constant folding.
                ASSERT_TRUE(IntervalReqExpr::getSingularDNF(*simplified));
            } else {
                // If we didn't use the real constant folding function, we have to constant fold
                // now, because our bounds will have If's.
                constFoldBounds(*simplified);
            }
            ASSERT(compareIntervals<N>(original, *simplified));
        } else {
            IntervalInclusionTransport<N> transport;
            ASSERT(transport.computeInclusion(original) == ExtendedBitset<N>());
        }
    }
}

// Generates a random integer bound. If isLow is true, lower values are more likely. If isLow is
// false, higher values are more likely.
template <int N, bool isLow>
ABT makeRandomIntBound(PseudoRandom& threadLocalRNG) {
    // This is a trick to create a skewed distribution on [0, N). Say N=3,
    // potential values of r = 0 1 2 3 4 5 6 7 8
    // (int) sqrt(r)         = 0 1 1 1 2 2 2 2 2
    // The higher the number is (as long as its <N), the more likely it is to occur.
    const int r = threadLocalRNG.nextInt32(N * N);
    const int bound = (int)std::sqrt(r);
    invariant(0 <= bound && bound < N);
    return Constant::int32(isLow ? N - 1 - bound : bound);
}

template <int N, bool isLow>
BoundRequirement makeRandomBound(PseudoRandom& threadLocalRNG,
                                 const std::vector<ProjectionName>& vars) {
    const bool isInclusive = threadLocalRNG.nextInt32(2);
    // We can return one of: N+2 constants (+2 because of minkey and maxkey), or 8 variables.
    const int r = threadLocalRNG.nextInt32(N + 10);
    if (r == 0) {
        return {isInclusive, Constant::minKey()};
    } else if (r == 1) {
        return {isInclusive, Constant::maxKey()};
    } else if (r < N + 2) {
        return {isInclusive, makeRandomIntBound<N, isLow>(threadLocalRNG)};
    } else {
        return {isInclusive, make<Variable>(vars.at(r - (N + 2)))};
    }
};

template <int N>
void testIntervalFuzz(const uint64_t seed, PseudoRandom& threadLocalRNG) {
    // Generate values for the eight variables we have.
    auto prefixId = PrefixId::createForTests();
    ProjectionNameMap<ABT> varMap;
    std::vector<ProjectionName> vars;
    for (size_t i = 0; i < 8; i++) {
        // minkey=0, maxkey=1, anything else is a constant
        const int type = threadLocalRNG.nextInt32(N + 2);
        ABT val = Constant::int32(type - 2);
        if (type == 0) {
            val = Constant::minKey();
        } else if (type == 1) {
            val = Constant::maxKey();
        }
        ProjectionName var = prefixId.getNextId("var");
        varMap.emplace(var.value(), val);
        vars.push_back(var);
    }
    EvalVariables varEval(std::move(varMap));

    // Create three intervals.
    constexpr size_t numIntervals = 3;

    // Intersect with multiple intervals.
    {
        IntervalReqExpr::NodeVector intervalVec;
        for (size_t i = 0; i < numIntervals; i++) {
            intervalVec.push_back(IntervalReqExpr::make<IntervalReqExpr::Atom>(
                IntervalRequirement{makeRandomBound<N, true>(threadLocalRNG, vars),
                                    makeRandomBound<N, false>(threadLocalRNG, vars)}));
        }

        auto original =
            IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
                IntervalReqExpr::make<IntervalReqExpr::Conjunction>(std::move(intervalVec))});
        auto simplified = intersectDNFIntervals(original, ConstEval::constFold);

        varEval.replaceVarsInInterval(original);
        if (simplified) {
            varEval.replaceVarsInInterval(*simplified);
            ASSERT(compareIntervals<N>(original, *simplified));
        } else {
            // 'simplified' false means the simplified interval is empty (always-false) and can't be
            // represented as a BoolExpr, so check that 'original' returns false on every example.
            IntervalInclusionTransport<N> transport;
            ASSERT(transport.computeInclusion(original) == ExtendedBitset<N>());
        }
    }

    // TODO SERVER-71175 should include a union test here

    // TODO SERVER-71175 should extend this to have DNF intervals that are not purely disjunctions
    // or purely conjunctions. A mix of ORs and ANDs seems necessary, to test that the two
    // simplification methods (intersecting and unioning) don't interfere with each other.
}

static constexpr int bitsetSize = 10;
static const size_t numThreads = ProcessInfo::getNumCores();

TEST_F(IntervalIntersection, IntervalPermutations) {
    // Number of permutations is bitsetSize^4 * 2^4 * 2
    // The first term is needed because we generate four bounds to intersect two intervals. The
    // second term is for the inclusivity of the four bounds. The third term is to determine if we
    // test with real constant folding or a no-op constant folding function.
    static constexpr int numPermutations =
        (bitsetSize * bitsetSize * bitsetSize * bitsetSize) * (2 * 2 * 2 * 2) * 2;
    /**
     * Test for interval intersection. Generate intervals with constants in the
     * range of [0, N), with random inclusion/exclusion of the endpoints. Intersect the intervals
     * and verify against ground truth.
     */
    std::cout << "Testing " << numPermutations << " interval permutations using " << numThreads
              << " cores...\n";
    auto timeBegin = Date_t::now();

    AtomicWord<int> permutation(0);
    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([&permutation]() {
            for (;;) {
                const int nextP = permutation.fetchAndAdd(1);
                if (nextP >= numPermutations) {
                    break;
                }
                testIntervalPermutation<bitsetSize>(nextP);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto elapsed =
        (Date_t::now().toMillisSinceEpoch() - timeBegin.toMillisSinceEpoch()) / 1000.0;
    std::cout << "...done. Took: " << elapsed << " s.\n";
}

TEST_F(IntervalIntersection, IntervalFuzz) {
    static constexpr int numFuzzTests = 5000;
    /**
     * Generate random intervals with a mix of variables and constants, and test that they intersect
     * and union correctly.
     */
    std::cout << "Testing " << numFuzzTests << " fuzzed intervals using " << numThreads
              << " cores...\n";
    const auto timeBeginFuzz = Date_t::now();

    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([]() {
            const auto seed = SecureRandom().nextInt64();
            std::cout << "Using random seed: " << seed << "\n";
            PseudoRandom threadLocalRNG(seed);
            for (size_t i = 0; i < numFuzzTests / numThreads; i++) {
                testIntervalFuzz<bitsetSize>(seed, threadLocalRNG);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto elapsedFuzz =
        (Date_t::now().toMillisSinceEpoch() - timeBeginFuzz.toMillisSinceEpoch()) / 1000.0;
    std::cout << "...done. Took: " << elapsedFuzz << " s.\n";
}

TEST(IntervalIntersection, IntersectionSpecialCase) {
    auto original = IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
        IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
            IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                {true, make<Variable>("var1")}, {true, make<Variable>("var1")}}),
            IntervalReqExpr::make<IntervalReqExpr::Atom>(IntervalRequirement{
                {false, make<Variable>("var2")}, {false, make<Variable>("var3")}})})});

    auto simplified = intersectDNFIntervals(original, ConstEval::constFold);
    ASSERT(simplified);

    EvalVariables varEval({
        {"var1", Constant::int32(3)},
        {"var2", Constant::int32(0)},
        {"var3", Constant::int32(3)},
    });
    varEval.replaceVarsInInterval(original);
    varEval.replaceVarsInInterval(*simplified);
    ASSERT(compareIntervals<bitsetSize>(original, *simplified));
}

}  // namespace
}  // namespace mongo::optimizer
