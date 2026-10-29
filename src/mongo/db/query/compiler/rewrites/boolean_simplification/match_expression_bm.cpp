// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"

#include <benchmark/benchmark.h>

namespace mongo::boolean_simplification {
using namespace std::literals::string_view_literals;
namespace {
std::unique_ptr<MatchExpression> buildOrOfLeafs(int value, std::vector<BSONObj>& bsonObjs) {
    BSONObj operand = BSON("$eq" << value);
    bsonObjs.push_back(operand);
    auto expr = std::make_unique<OrMatchExpression>();
    expr->add(std::make_unique<EqualityMatchExpression>("a"sv, operand["$eq"]));
    expr->add(std::make_unique<EqualityMatchExpression>("b"sv, operand["$eq"]));
    expr->add(std::make_unique<EqualityMatchExpression>("c"sv, operand["$eq"]));
    return expr;
}

std::unique_ptr<MatchExpression> buildAndOfOrs(int startValue,
                                               int endValue,
                                               std::vector<BSONObj>& bsonObj) {
    auto expr = std::make_unique<AndMatchExpression>();
    for (int value = startValue; value < endValue; ++value) {
        expr->add(buildOrOfLeafs(value, bsonObj));
    }
    return expr;
}

struct EnableSimplifier {};
struct DisableSimplifier {};
}  // namespace

/**
 * 1 Minterm of size N
 */
void matchExpression_createAnd(benchmark::State& state) {
    const size_t numPredicates = static_cast<size_t>(state.range());
    std::vector<BSONObj> operands;
    operands.reserve(numPredicates);
    for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
        operands.emplace_back(BSON("a" << static_cast<int>(predicateIndex)));
    }

    for (auto _ : state) {
        auto root = std::make_unique<AndMatchExpression>();

        for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
            root->add(
                std::make_unique<EqualityMatchExpression>("a"sv, operands[predicateIndex]["a"]));
        }
    }
}

BENCHMARK(matchExpression_createAnd)->RangeMultiplier(10)->Range(10, 10000);

void matchExpression_createOr(benchmark::State& state) {
    const size_t numPredicates = static_cast<size_t>(state.range());
    std::vector<BSONObj> operands;
    operands.reserve(numPredicates);
    for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
        operands.emplace_back(BSON("a" << static_cast<int>(predicateIndex)));
    }

    for (auto _ : state) {
        auto root = std::make_unique<OrMatchExpression>();

        for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
            root->add(
                std::make_unique<EqualityMatchExpression>("a"sv, operands[predicateIndex]["a"]));
        }
    }
}

BENCHMARK(matchExpression_createOr)->RangeMultiplier(10)->Range(10, 10000);

void matchExpression_createAndOfOrs(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range());
    std::vector<BSONObj> operands;
    operands.reserve(size);
    for (size_t index = 0; index < size; ++index) {
        operands.emplace_back(BSON("a" << static_cast<int>(index)));
    }

    for (auto _ : state) {
        auto root = std::make_unique<AndMatchExpression>();
        for (size_t index = 0; index < size; ++index) {
            auto orExpr = std::make_unique<OrMatchExpression>();
            orExpr->add(std::make_unique<EqualityMatchExpression>("a"sv, operands[index]["a"]));
            orExpr->add(std::make_unique<GTMatchExpression>("a"sv, operands[index]["a"]));
            root->add(std::move(orExpr));
        }
    }
}

BENCHMARK(matchExpression_createAndOfOrs)->Args({3})->Args({7})->Args({10})->Args({13});

void matchExpression_cloneAnd(benchmark::State& state) {
    const size_t numPredicates = static_cast<size_t>(state.range());
    auto root = std::make_unique<AndMatchExpression>();
    std::vector<BSONObj> operands;
    operands.reserve(numPredicates);
    for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
        operands.emplace_back(BSON("a" << static_cast<int>(predicateIndex)));
        root->add(std::make_unique<EqualityMatchExpression>("a"sv, operands.back()["a"]));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(root->clone());
    }
}

BENCHMARK(matchExpression_cloneAnd)->RangeMultiplier(10)->Range(10, 10000);

void matchExpression_cloneOr(benchmark::State& state) {
    const size_t numPredicates = static_cast<size_t>(state.range());
    auto root = std::make_unique<OrMatchExpression>();
    std::vector<BSONObj> operands;
    operands.reserve(numPredicates);
    for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
        operands.emplace_back(BSON("a" << static_cast<int>(predicateIndex)));
        root->add(std::make_unique<EqualityMatchExpression>("a"sv, operands.back()["a"]));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(root->clone());
    }
}

BENCHMARK(matchExpression_cloneOr)->RangeMultiplier(10)->Range(10, 10000);

void matchExpression_cloneAndOfOrs(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range());
    auto root = std::make_unique<AndMatchExpression>();
    std::vector<BSONObj> operands;
    operands.reserve(size);
    for (size_t index = 0; index < size; ++index) {
        auto orExpr = std::make_unique<OrMatchExpression>();
        operands.emplace_back(BSON("a" << static_cast<int>(index)));
        orExpr->add(std::make_unique<EqualityMatchExpression>("a"sv, operands.back()["a"]));
        orExpr->add(std::make_unique<GTMatchExpression>("a"sv, operands.back()["a"]));
        root->add(std::move(orExpr));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(root->clone());
    }
}

BENCHMARK(matchExpression_cloneAndOfOrs)->Args({3})->Args({7})->Args({10})->Args({13});

/**
 * Trivially simple expression (1 predicate).
 */
template <typename SimplifierStatus>
void matchExpressionOptimize_triviallySimple(benchmark::State& state) {
    auto operand = BSON("$eq" << 1);
    auto expr = std::make_unique<EqualityMatchExpression>("a"sv, operand["$eq"]);

    for (auto _ : state) {
        benchmark::DoNotOptimize(optimizeMatchExpression(
            expr->clone(), std::is_same_v<EnableSimplifier, SimplifierStatus>));
    }
}

BENCHMARK_TEMPLATE(matchExpressionOptimize_triviallySimple, DisableSimplifier);
BENCHMARK_TEMPLATE(matchExpressionOptimize_triviallySimple, EnableSimplifier);

/**
 * Simple $and of 3 predicates expression.
 */
template <typename SimplifierStatus>
void matchExpressionOptimize_simpleAnd(benchmark::State& state) {
    auto operand = BSON("$eq" << 1);
    auto expr = std::make_unique<AndMatchExpression>();
    expr->add(std::make_unique<EqualityMatchExpression>("a"sv, operand["$eq"]));
    expr->add(std::make_unique<EqualityMatchExpression>("b"sv, operand["$eq"]));
    expr->add(std::make_unique<EqualityMatchExpression>("c"sv, operand["$eq"]));

    for (auto _ : state) {
        benchmark::DoNotOptimize(optimizeMatchExpression(
            expr->clone(), std::is_same_v<EnableSimplifier, SimplifierStatus>));
    }
}

BENCHMARK_TEMPLATE(matchExpressionOptimize_simpleAnd, DisableSimplifier);
BENCHMARK_TEMPLATE(matchExpressionOptimize_simpleAnd, EnableSimplifier);

/**
 * Simple $or of 3 predicates expression.
 */
template <typename SimplifierStatus>
void matchExpressionOptimize_simpleOr(benchmark::State& state) {
    auto operand = BSON("$eq" << 1);
    auto expr = std::make_unique<OrMatchExpression>();
    expr->add(std::make_unique<EqualityMatchExpression>("a"sv, operand["$eq"]));
    expr->add(std::make_unique<EqualityMatchExpression>("b"sv, operand["$eq"]));
    expr->add(std::make_unique<EqualityMatchExpression>("c"sv, operand["$eq"]));

    for (auto _ : state) {
        benchmark::DoNotOptimize(optimizeMatchExpression(
            expr->clone(), std::is_same_v<EnableSimplifier, SimplifierStatus>));
    }
}

BENCHMARK_TEMPLATE(matchExpressionOptimize_simpleOr, DisableSimplifier);
BENCHMARK_TEMPLATE(matchExpressionOptimize_simpleOr, EnableSimplifier);

/**
 * Moderately complex expression that ended up in 81 minterms.
 */
template <typename SimplifierStatus>
void matchExpressionOptimize_mediumComplex(benchmark::State& state) {
    auto expr = std::make_unique<AndMatchExpression>();
    std::vector<BSONObj> bsonObjs;
    expr->add(buildAndOfOrs(0, 3, bsonObjs));
    expr->add(buildAndOfOrs(3, 6, bsonObjs));
    expr->add(buildAndOfOrs(6, 9, bsonObjs));

    for (auto _ : state) {
        benchmark::DoNotOptimize(optimizeMatchExpression(
            expr->clone(), std::is_same_v<EnableSimplifier, SimplifierStatus>));
    }
}

BENCHMARK_TEMPLATE(matchExpressionOptimize_mediumComplex, DisableSimplifier);
BENCHMARK_TEMPLATE(matchExpressionOptimize_mediumComplex, EnableSimplifier);

/**
 * Maximum allowed complex expression that ended up in 486 minterms.
 */
template <typename SimplifierStatus>
void matchExpressionOptimize_maxComplex(benchmark::State& state) {
    auto expr = std::make_unique<AndMatchExpression>();
    std::vector<BSONObj> bsonObjs;
    {
        auto orExpr = std::make_unique<OrMatchExpression>();
        orExpr->add(buildAndOfOrs(0, 3, bsonObjs));
        orExpr->add(buildAndOfOrs(3, 6, bsonObjs));
        orExpr->add(buildAndOfOrs(6, 9, bsonObjs));
        expr->add(std::move(orExpr));
    }

    {
        auto operand = BSON("$gt" << 0);
        bsonObjs.push_back(operand);
        auto orExpr = std::make_unique<OrMatchExpression>();
        orExpr->add(std::make_unique<GTMatchExpression>("a"sv, operand["$gt"]));
        orExpr->add(std::make_unique<GTMatchExpression>("b"sv, operand["$gt"]));
        orExpr->add(std::make_unique<GTMatchExpression>("c"sv, operand["$gt"]));
        orExpr->add(std::make_unique<GTMatchExpression>("d"sv, operand["$gt"]));
        orExpr->add(std::make_unique<GTMatchExpression>("e"sv, operand["$gt"]));
        orExpr->add(std::make_unique<GTMatchExpression>("f"sv, operand["$gt"]));
        expr->add(std::move(orExpr));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(optimizeMatchExpression(
            expr->clone(), std::is_same_v<EnableSimplifier, SimplifierStatus>));
    }
}

BENCHMARK_TEMPLATE(matchExpressionOptimize_maxComplex, DisableSimplifier);
BENCHMARK_TEMPLATE(matchExpressionOptimize_maxComplex, EnableSimplifier);

/**
 * Too complex expression that abort the simplification earlier.
 */
template <typename SimplifierStatus>
void matchExpressionOptimize_overComplex(benchmark::State& state) {
    std::vector<BSONObj> bsonObjs;
    auto expr = std::make_unique<AndMatchExpression>();
    {
        auto orExpr = std::make_unique<OrMatchExpression>();
        orExpr->add(buildAndOfOrs(0, 3, bsonObjs));
        orExpr->add(buildAndOfOrs(3, 6, bsonObjs));
        orExpr->add(buildAndOfOrs(6, 9, bsonObjs));
        expr->add(std::move(orExpr));
    }
    {
        auto orExpr = std::make_unique<OrMatchExpression>();
        orExpr->add(buildAndOfOrs(9, 12, bsonObjs));
        orExpr->add(buildAndOfOrs(12, 15, bsonObjs));
        orExpr->add(buildAndOfOrs(15, 18, bsonObjs));
        expr->add(std::move(orExpr));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(optimizeMatchExpression(
            expr->clone(), std::is_same_v<EnableSimplifier, SimplifierStatus>));
    }
}

BENCHMARK_TEMPLATE(matchExpressionOptimize_overComplex, DisableSimplifier);
BENCHMARK_TEMPLATE(matchExpressionOptimize_overComplex, EnableSimplifier);
}  // namespace mongo::boolean_simplification
