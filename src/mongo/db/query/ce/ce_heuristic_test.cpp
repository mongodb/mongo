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

#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/cost_derivation.h"
#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace optimizer;
using namespace optimizer::cascades;

constexpr double kCollCard = 10000.0;
const std::string collName = "test";

class HeuristicCETester : public CETester {
public:
    HeuristicCETester(std::string collName) : CETester(collName, 0) {}

protected:
    std::unique_ptr<CEInterface> getCETransport() const override {
        return std::make_unique<HeuristicCE>();
    }
};

HeuristicCETester ht(collName);

TEST(CEHeuristicTest, CEwithoutOptimization) {
    std::string query = "{a0 : {$gt : 14, $lt : 21}}";
    ht.setCollCard(kCollCard);
    double ce = ht.getCE(query, 0);
    ASSERT_APPROX_EQUAL(100.0, ce, kMaxCEError);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_Eq) {
    std::string query = "{a : 123}";
    ASSERT_MATCH_CE_CARD(ht, query, 0.0, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.73205, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 2.64575, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.16228, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 10.0, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 100.0, 10000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_Gt) {
    std::string query = "{a: {$gt: 44}}";
    ASSERT_MATCH_CE_CARD(ht, query, 0.01, 0.0);
    ASSERT_MATCH_CE_CARD(ht, query, 0.7, 1.0);
    ASSERT_MATCH_CE_CARD(ht, query, 6.3, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 44.55, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 330, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_Gt_Lt) {
    std::string query = "{a: {$gt: 44, $lt: 99}}";
    ASSERT_MATCH_CE_CARD(ht, query, 0.585662, 1.0);
    ASSERT_MATCH_CE_CARD(ht, query, 5.27096, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 29.885, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 189.571, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_AND2Eq) {
    std::string query = "{a : 13, b : 42}";
    ASSERT_MATCH_CE_CARD(ht, query, 1.31607, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.62658, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.77828, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.16228, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 10.0, 10000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_AND3Eq) {
    std::string query = "{a : 13, b : 42, c : 69}";
    ASSERT_MATCH_CE_CARD(ht, query, 1.1472, 3.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.27537, 7.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.33352, 10.0);
    ASSERT_MATCH_CE_CARD(ht, query, 1.77828, 100.0);
    ASSERT_MATCH_CE_CARD(ht, query, 3.16228, 10000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_OR1path) {
    std::string query = "{$or: [{a0: {$gt: 44}}, {a0: {$lt: 9}}]}";
    ASSERT_MATCH_CE_CARD(ht, query, 7.52115, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 58.6188, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 451.581, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_OR2paths) {
    std::string query = "{$or: [{a0: {$gt:44}}, {b0: {$lt: 9}}]}";
    // TODO: Disjunctions on different paths are not SARGable, and are represented as a single
    // FilterNode. FilterNode is currenlty estimated to be always 10% selectivity.
    ASSERT_MATCH_CE_CARD(ht, query, 0.9, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 9.9, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 100.0, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_DNF1pathSimple) {
    std::string query =
        "{$or: ["
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 44}}]}"
        "]}";
    ASSERT_MATCH_CE_CARD(ht, query, 6.59965, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 41.2515, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 270.42, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_DNF1pathComplex) {
    // Each disjunct has different number of conjuncts, so that its selectivity is different.
    // We need 5 disjuncts to test exponential backoff which cuts off at the first 4.
    // The conjuncts are in selectivity order.
    std::string query1 =
        "{$or: ["
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]},"
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}, {a0: {$gt: 42}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}, {a0: {$lt: 77}}]}"
        "]}";
    auto ce1 = ht.getCE(query1);
    // The conjuncts are in inverse selectivity order.
    std::string query2 =
        "{$or: ["
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}, {a0: {$lt: 77}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}, {a0: {$lt: "
        "81}}]},"
        "{$and: [{a0: {$gt:40}}, {a0: {$lt: 99}}, {a0: {$gt: 42}}, {a0: {$lt: 88}}]},"
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}, {a0: {$gt: 42}}]},"
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]}"
        "]}";
    auto ce2 = ht.getCE(query2);
    ASSERT_APPROX_EQUAL(ce1, ce2, kMaxCEError);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_DNF2paths) {
    // TODO: Disjunctions on different paths are not SARGable, and are represented as a single
    // FilterNode. FilterNode is currently estimated to be always 10% selectivity.
    std::string query =
        "{$or: ["
        "{$and: [{a0: {$gt: 9}}, {a0: {$lt: 12}}]},"
        "{$and: [{b0: {$gt:40}}, {b0: {$lt: 44}}]}"
        "]}";
    ASSERT_MATCH_CE_CARD(ht, query, 0.9, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 9.9, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 100.0, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_CNF1path) {
    std::string query =
        "{$and : ["
        "{$or : [ {a0 : {$gt : 11}}, {a0 : {$lt : 44}} ]},"
        "{$or : [ {a0 : {$gt : 77}}, {a0 : {$eq : 51}} ]}"
        "]}";
    ASSERT_MATCH_CE_CARD(ht, query, 6.87663, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 42.7435, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 275.419, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionPhase_CNF2paths) {
    std::string query =
        "{$and : ["
        "{$or : [ {a0 : {$gt : 11}}, {a0 : {$lt : 44}} ]},"
        "{$or : [ {b0 : {$gt : 77}}, {b0 : {$eq : 51}} ]}"
        "]}";
    ASSERT_MATCH_CE_CARD(ht, query, 6.21212, 9.0);
    ASSERT_MATCH_CE_CARD(ht, query, 36.4418, 99.0);
    ASSERT_MATCH_CE_CARD(ht, query, 228.935, 1000.0);
}

TEST(CEHeuristicTest, CEAfterMemoSubstitutionExplorationPhases) {
    std::string query = "{a : 13, b : 42}";
    ht.setCollCard(kCollCard);
    double ce = ht.getCE(query, 2);
    ASSERT_APPROX_EQUAL(10.0, ce, kMaxCEError);
}

}  // namespace
}  // namespace mongo::ce
