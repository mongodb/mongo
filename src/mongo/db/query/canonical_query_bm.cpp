/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <benchmark/benchmark.h>
#include <random>
#include <vector>

#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const NamespaceString kNss =
    NamespaceString::createNamespaceString_forTest("canonical_query_init_test.bm");

static constexpr StringData kCollName = "exampleCol"_sd;
static constexpr StringData kDbName = "foo"_sd;
static constexpr uint64_t kRandomSeed = 619449996;

enum QueryType {
    kFindWithSimplePredicates,
    kFindWithInPredicates,
    kFindWithIdPredicates,
    kFindWithNestedOrPredicates,
    kFindWithNestedAndPredicates,
    kFindWithNestedAndOrPredicates,
};

enum LogicalOperator { kOr, kAnd, kMix };

enum DottedPathType { kSimple, kNested };

struct FindQueryParameters {
    int numDistinctMatchStatements;
    int numOfDistinctValues;
    int numOfDistinctFields;
    int numOfRepetitionsPerStatement;
    bool dottedPaths;
    DottedPathType dottedPathType;
    int dottedPathLength;
};

struct InArrayQueryParameters {
    int sizeOfInArray;
};

struct NestedLogicalOpsParameters {
    int numOfNestings;
};

struct CanonicalQueryBenchmarkParameters {
    QueryType queryType;
    FindQueryParameters findQueryParams;
    InArrayQueryParameters inArrayParams;
    NestedLogicalOpsParameters nestedOpsParams;

    CanonicalQueryBenchmarkParameters(benchmark::State& state)
        : queryType(static_cast<QueryType>(state.range(0))),
          findQueryParams({static_cast<int>(state.range(1)),
                           static_cast<int>(state.range(2)),
                           static_cast<int>(state.range(3)),
                           static_cast<int>(state.range(4)),
                           static_cast<bool>(state.range(5)),
                           static_cast<DottedPathType>(state.range(6)),
                           static_cast<int>(state.range(7))}),
          inArrayParams({static_cast<int>(state.range(8))}),
          nestedOpsParams({static_cast<int>(state.range(9))}) {}
};

/**
 * Create a predicate on a new field and add it to the provided query builder. The method is
 * templated to accept different types of predicates e.g., the predicate can be a simple integer or
 * a complex BSONObj.
 */
template <typename T>
void addPredicatesIntoBSONObjectBuilder(BSONObjBuilder& filter,
                                        std::vector<std::string> fieldName,
                                        T in,
                                        int repeat = 1,
                                        bool dottedPaths = false,
                                        DottedPathType dottedPathType = DottedPathType::kSimple) {
    std::string finalFieldName = fieldName[0];

    if (dottedPaths && dottedPathType == DottedPathType::kSimple) {
        for (unsigned int i = 1; i < fieldName.size(); i++) {
            finalFieldName += "." + fieldName[i];
        }
    }

    // initial
    BSONObjBuilder inner;
    inner.append(finalFieldName, in);
    BSONObj innerObj = inner.obj();

    if (dottedPaths && dottedPathType == DottedPathType::kNested) {
        for (unsigned int i = 1; i < fieldName.size(); i++) {
            BSONObjBuilder builder;
            builder.append(fieldName[i], innerObj);
            innerObj = builder.obj();
        }
    }

    for (int i = 0; i < repeat; i++) {
        filter.appendElements(innerObj);
    }
}

/**
 * Generate a sequence of random integers within specific bounds. The method accepts as input the
 * number of distinct values to generate as well as the total number of values to generate.
 */
std::vector<int> generateDistinctValues(uint64_t numOfDistinctValues,
                                        uint64_t numOfValues,
                                        uint64_t min = 0,
                                        uint64_t max = 1000000) {
    // Create a vector to hold the random numbers
    std::vector<int> results;

    uassert(8581501,
            "The number of requested distinct values must exist within the provided interval",
            numOfDistinctValues <= max);

    std::default_random_engine eng{kRandomSeed};
    std::mt19937 rng(eng());
    std::uniform_int_distribution<std::mt19937::result_type> distrib(min, max);

    // Generate random numbers until we've reached the desired amount of distinct values
    while (results.size() != numOfDistinctValues) {
        // Generate a random number within the range [min, max]
        uint64_t num = distrib(rng);

        // If the number doesn't exist yet, add it to the vector
        if (find(results.begin(), results.end(), num) == results.end()) {
            results.push_back(num);
        }
    }

    // fill the remaining required values from the already populated distinct ones.
    std::uniform_int_distribution<std::mt19937::result_type> distrib2(0, numOfDistinctValues - 1);
    if (numOfValues > numOfDistinctValues) {
        for (unsigned int i = 0; i < (numOfValues - numOfDistinctValues); i++) {
            results.push_back(results[distrib2(rng)]);
        }
    }

    return results;
}

/**
 * Generate simple field names or in case of dotted paths a vector of field names.
 */
std::vector<std::vector<std::string>> generateFieldNames(uint64_t numOfDistinctFields,
                                                         uint64_t noOfValues,
                                                         bool dottedPath = false,
                                                         int dottedPathLength = 1) {
    std::vector<std::vector<std::string>> result;
    static constexpr StringData kViableChars = "abcdefghijklmnopqrstuvwxyz"_sd;
    dottedPathLength = (dottedPathLength == 0) ? 1 : dottedPathLength;
    std::vector<int> ids = generateDistinctValues(
        numOfDistinctFields, noOfValues * dottedPathLength, 0, kViableChars.size() - 1);

    int dottedPathI = 0;
    std::vector<std::string> currentPath;
    for (unsigned int i : ids) {

        currentPath.push_back(std::string(1, kViableChars[(int)i]));
        dottedPathI++;

        if (dottedPath && (dottedPathI < dottedPathLength)) {
            continue;
        }

        result.push_back(currentPath);
        currentPath.clear();
        dottedPathI = 0;
    }

    if (!currentPath.empty()) {
        result.push_back(currentPath);
    }

    return result;
}

/**
 * Generate a simple matching expression for find commands.
 * The function generates a sequence of values and fields and creates a set of filter expressions.
 */
BSONObj generateSimpleFindPredicates(int numOfDistinctValues,
                                     int numOfDistinctFields,
                                     int numDistinctMatchStatements,
                                     int numOfRepetitionsPerStatement,
                                     bool idQuery = false,
                                     bool dottedPaths = false,
                                     DottedPathType dottedPathType = DottedPathType::kSimple,
                                     int dottedPathLength = 1) {

    std::vector<int> values =
        generateDistinctValues(numOfDistinctValues, numDistinctMatchStatements);

    std::vector<std::vector<std::string>> fields(numDistinctMatchStatements,
                                                 std::vector<std::string>(1, "_id"));

    if (!idQuery) {
        fields = generateFieldNames(
            numOfDistinctFields, numDistinctMatchStatements, dottedPaths, dottedPathLength);
    }

    BSONObjBuilder filter;
    for (int i = 0; i < numDistinctMatchStatements; i++) {
        addPredicatesIntoBSONObjectBuilder(filter,
                                           fields[i],
                                           values[i],
                                           numOfRepetitionsPerStatement,
                                           dottedPaths,
                                           dottedPathType);
    }

    return filter.obj();
}

/**
 * Generate a nested logical or/and expression based on the given parameters.
 * The logical operator input may lead to a random generation of and/or nestings.
 */
BSONObj generateNestedLogicalOperatorPredicates(int numOfDistinctValues,
                                                int numOfDistinctFields,
                                                int numDistinctMatchStatements,
                                                int numOfRepetitionsPerStatement,
                                                int numOfNestings,
                                                LogicalOperator logicalOperator) {

    std::default_random_engine eng{kRandomSeed};
    std::mt19937 rng(eng());
    std::uniform_int_distribution<std::mt19937::result_type> distrib(0, 1);

    BSONObj buildUpNesting = generateSimpleFindPredicates(numOfDistinctValues,
                                                          numOfDistinctFields,
                                                          numDistinctMatchStatements,
                                                          numOfRepetitionsPerStatement);

    for (int i = 0; i < numOfNestings; i++) {

        BSONObj inner2 = generateSimpleFindPredicates(numOfDistinctValues,
                                                      numOfDistinctFields,
                                                      numDistinctMatchStatements,
                                                      numOfRepetitionsPerStatement);

        std::vector<BSONObj> newStatement = {buildUpNesting, inner2};
        BSONObjBuilder currentOr;

        std::string logicalOperatorStr;
        switch (logicalOperator) {
            case LogicalOperator::kAnd:
                logicalOperatorStr = "$and";
                break;
            case LogicalOperator::kOr:
                logicalOperatorStr = "$or";
                break;
            case LogicalOperator::kMix:
                logicalOperatorStr = (distrib(rng) == 0) ? "$and" : "$or";
                break;
        }

        currentOr.append(logicalOperatorStr, newStatement);

        buildUpNesting = currentOr.obj();
    }

    return buildUpNesting;
}

/**
 * Generate a find query to use as input for performance testing.
 * Depending on the specific type of query to generate, the input parameters may be partially
 * populated.
 */
BSONObj getQueryTemplate(CanonicalQueryBenchmarkParameters params) {

    switch (params.queryType) {
        // Generating queries with simple predicates e.g.,
        // for numOfDistinctValues:3 and numOfDistinctFields:3
        // find( {a: 1, b: 2, c: 3})
        case QueryType::kFindWithSimplePredicates: {
            // Find query with varying number of predicates.

            BSONObj filterStatement =
                generateSimpleFindPredicates(params.findQueryParams.numOfDistinctValues,
                                             params.findQueryParams.numOfDistinctFields,
                                             params.findQueryParams.numDistinctMatchStatements,
                                             params.findQueryParams.numOfRepetitionsPerStatement,
                                             /*idQuery*/ false,
                                             params.findQueryParams.dottedPaths,
                                             params.findQueryParams.dottedPathType,
                                             params.findQueryParams.dottedPathLength);

            return BSON("find" << kCollName << "$db" << kDbName << "filter" << filterStatement);
        }
        // Generating queries with in predicates e.g.,
        // for numOfDistinctValues:3 and sizeOfInArray:5
        // find( {a: {$in: [1,2,3,2,1]}})
        case QueryType::kFindWithInPredicates: {
            // Find query with 'in' predicate.

            BSONObjBuilder filter;

            std::vector<std::vector<std::string>> fields = generateFieldNames(
                /*numOfDistinctFields*/ params.findQueryParams.numOfDistinctFields,
                /*noOfValues*/ params.findQueryParams.numDistinctMatchStatements,
                /*dottedPath*/ false,
                /*dottedPathLength*/ 1);

            // generating here all the values to allow having a specific seed for random number
            // while creating different values in each array.
            std::vector<int> values =
                generateDistinctValues(params.findQueryParams.numOfDistinctValues,
                                       params.inArrayParams.sizeOfInArray *
                                           params.findQueryParams.numDistinctMatchStatements);

            for (int array = 0; array < params.findQueryParams.numDistinctMatchStatements;
                 array++) {

                std::vector<int> curValues(
                    values.begin() + array * params.inArrayParams.sizeOfInArray,
                    values.begin() + ((array + 1) * params.inArrayParams.sizeOfInArray));

                BSONObjBuilder in;
                BSONObj inObj = in.append("$in", curValues).obj();
                for (int repeat = 0; repeat < params.findQueryParams.numOfRepetitionsPerStatement;
                     repeat++) {
                    addPredicatesIntoBSONObjectBuilder(filter, fields[array], inObj);
                }
            }

            return BSON("find" << kCollName << "$db" << kDbName << "filter" << filter.obj());
        }
        // Generating queries with in predicates e.g.,
        // for numOfDistinctValues:2
        // find( {_id: 1, _id: 3} )
        case QueryType::kFindWithIdPredicates: {
            // Find query on predicate on _id.

            BSONObj filterStatement =
                generateSimpleFindPredicates(params.findQueryParams.numOfDistinctValues,
                                             params.findQueryParams.numOfDistinctFields,
                                             params.findQueryParams.numDistinctMatchStatements,
                                             params.findQueryParams.numOfRepetitionsPerStatement,
                                             /*idQuery*/ true);

            return BSON("find" << kCollName << "$db" << kDbName << "filter" << filterStatement);
        }
        // Generating queries with in predicates e.g.,
        // for numOfNestings:2
        // find( {$or: [{$or: [{a: 1}, {b:2}}, {c:3}]})
        case QueryType::kFindWithNestedOrPredicates: {
            // Find query with nested '$or' predicates.

            BSONObj nestedOrs = generateNestedLogicalOperatorPredicates(
                params.findQueryParams.numOfDistinctValues,
                params.findQueryParams.numOfDistinctFields,
                params.findQueryParams.numDistinctMatchStatements,
                params.findQueryParams.numOfRepetitionsPerStatement,
                params.nestedOpsParams.numOfNestings,
                /*logicalOperator*/ LogicalOperator::kOr);

            return BSON("find" << kCollName << "$db" << kDbName << "filter" << nestedOrs);
        }
        // Generating queries with in predicates e.g.,
        // for numOfNestings:2
        // find( {$and: [{$and: [{a: 1}, {b:2}}, {c:3}]})
        case QueryType::kFindWithNestedAndPredicates: {
            // Find query with nested '$and' predicates.

            BSONObj nestedAnds = generateNestedLogicalOperatorPredicates(
                params.findQueryParams.numOfDistinctValues,
                params.findQueryParams.numOfDistinctFields,
                params.findQueryParams.numDistinctMatchStatements,
                params.findQueryParams.numOfRepetitionsPerStatement,
                params.nestedOpsParams.numOfNestings,
                /*logicalOperator*/ LogicalOperator::kAnd);

            return BSON("find" << kCollName << "$db" << kDbName << "filter" << nestedAnds);
        }
        // Generating queries with in predicates e.g.,
        // for numOfNestings:2
        // find( {$and: [{$or: [{a: 1}, {b:2}}, {c:3}]})
        case QueryType::kFindWithNestedAndOrPredicates: {
            // Find query with nested '$and/$or' predicates.

            BSONObj nestedLogicalOps = generateNestedLogicalOperatorPredicates(
                params.findQueryParams.numOfDistinctValues,
                params.findQueryParams.numOfDistinctFields,
                params.findQueryParams.numDistinctMatchStatements,
                params.findQueryParams.numOfRepetitionsPerStatement,
                params.nestedOpsParams.numOfNestings,
                /*logicalOperator*/ LogicalOperator::kMix);

            return BSON("find" << kCollName << "$db" << kDbName << "filter" << nestedLogicalOps);

            break;
        }
    }

    tassert(8581500, "All benchmark invocations should have defined query type", true);
    return BSONObj();
}

void BM_CreateCanonicalQuery(benchmark::State& state) {

    CanonicalQueryBenchmarkParameters params(state);

    BSONObj query = getQueryTemplate(params);

    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContextForTest> expCtx =
        new ExpressionContextForTest(opCtx.get(), kNss);

    for (auto curState : state) {
        state.PauseTiming();

        auto findCommandRequest = query_request_helper::makeFromFindCommandForTests(query);

        auto parsedRequest = uassertStatusOK(parsed_find_command::parse(
            expCtx,
            {.findCommand = std::move(findCommandRequest),
             .extensionsCallback = ExtensionsCallbackReal(opCtx.get(), &kNss),
             .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

        CanonicalQueryParams canQueryParams{.expCtx = expCtx,
                                            .parsedFind = std::move(parsedRequest)};

        state.ResumeTiming();

        auto cq = std::make_unique<CanonicalQuery>(std::move(canQueryParams));
    }
}

/**
 * This benchmark allows varying:
 * the type of predicates: kFindWithSimplePredicates, kFindWithInPredicates, kFindWithInPredicates,
 *  kFindWithNestedOrPredicates, kFindWithNestedAndPredicates, kFindWithNestedAndOrPredicates
 * numDistinctMatchStatements: number of match predicate in the query.
 * numOfDistinctValues: number of distinct values to compare against.
 * numOfDistinctFields: number of distinct fields to compare against (max: 26)
 * numOfRepetitionsPerStatement: number of identically repeated predicates (to increase the overall
 *  number of predicates).
 * dottedPaths: the complexity of the paths in the predicates (dotted or not)
 * dottedPathType: the type of dotted paths: simple e.g., {a.b.c: 1} / nested {a: { b: { c: 1}}}
 * dottedPathLength: the length of the dotted path.
 * sizeOfInArray: in case of an "in" predicate the size of the array to compare against.
 * numOfNestings: In case of nested logical predicates, the depth of nestings.
 *
 * Due to the constraint on distinct fields, the total number of distinct fields including dotted
 * paths appearing in the query cannot be more than 26.
 */

// long match statement with simple predicates
// { find: "exampleCol", $db: "foo", filter:
// { j: 1, j: 1, n: 2, n: 2, i: 3, i: 3, v: 4, v: 4, v: 4, v: 4,
//   j: 1, j: 1, i: 3, i: 3, n: 2, n: 2, i: 3, i: 3, n: 2, n: 2 } }
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithSimplePredicates,
            /*numDistinctMatchStatements*/ 10,
            /*numOfDistinctValues*/ 4,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 0,
            /*dottedPathType*/ DottedPathType::kSimple,
            /*dottedPathLength*/ 0,
            /*sizeOfInArray*/ 0,
            /*numOfNestings*/ 0});

// nested paths -- nested
// { find: "exampleCol", $db: "foo", filter:
// { n: { i: { j: { v: { v: { i: { n: { j: 365456 } } } } } } },
//   n: { i: { j: { v: { v: { i: { n: { j: 365456 } } } } } } },
//   n: { n: { v: { j: { n: { j: { n: { i: 517521 } } } } } } },
//   n: { n: { v: { j: { n: { j: { n: { i: 517521 } } } } } } },
//   i: { n: { n: { v: { v: { i: { j: { i: 334239 } } } } } } },
//   i: { n: { n: { v: { v: { i: { j: { i: 334239 } } } } } } },
//   j: { v: { n: { n: { v: { j: { n: { v: 841725 } } } } } } },
//   j: { v: { n: { n: { v: { j: { n: { v: 841725 } } } } } } } } }
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithSimplePredicates,
            /*numDistinctMatchStatements*/ 4,
            /*numOfDistinctValues*/ 4,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 1,
            /*dottedPathType*/ DottedPathType::kNested,
            /*dottedPathLength*/ 8,
            /*sizeOfInArray*/ 0,
            /*numOfNestings*/ 0});

// nested paths -- simple
// { find: "exampleCol", $db: "foo", filter:
// { j.n.i.v.v.j: 365456, j.n.i.v.v.j: 365456, i.n.i.n.j.n: 517521, i.n.i.n.j.n: 517521,
//   j.v.n.n.i.j: 334239, j.v.n.n.i.j: 334239, i.v.v.n.n.i: 841725, i.v.v.n.n.i: 841725,
//   v.n.j.v.n.n: 841725, v.n.j.v.n.n: 841725, v.j.v.n.n.v: 365456, v.j.v.n.n.v: 365456,
//   j.j.j.j.v.i: 334239, j.j.j.j.v.i: 334239, v.j.j.j.n.i: 517521, v.j.j.j.n.i: 517521 } }
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithSimplePredicates,
            /*numDistinctMatchStatements*/ 8,
            /*numOfDistinctValues*/ 4,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 1,
            /*dottedPathType*/ DottedPathType::kSimple,
            /*dottedPathLength*/ 6,
            /*sizeOfInArray*/ 0,
            /*numOfNestings*/ 0});

// $in filters with large arrays
// { find: "exampleCol", $db: "foo", filter:
// { j: { $in: [ 365456, 517521, 334239, 841725, 872729, 365456, 334239, 517521, 841725, 334239 ] },
//   j: { $in: [ 365456, 517521, 334239, 841725, 872729, 365456, 334239, 517521, 841725, 334239 ] },
//   n: { $in: [ 365456, 517521, 365456, 872729, 517521, 517521, 334239, 365456, 841725, 841725 ] },
//   n: { $in: [ 365456, 517521, 365456, 872729, 517521, 517521, 334239, 365456, 841725, 841725 ] },
//   i: { $in: [ 872729, 517521, 334239, 334239, 872729, 517521, 365456, 872729, 517521, 517521 ] },
//   i: { $in: [ 872729, 517521, 334239, 334239, 872729, 517521, 365456, 872729, 517521, 517521 ] },
//   v: { $in: [ 872729, 365456, 872729, 334239, 517521, 841725, 517521, 365456, 517521, 365456 ] },
//   v: { $in: [ 872729, 365456, 872729, 334239, 517521, 841725, 517521, 365456, 517521, 365456 ] },
//   v: { $in: [ 872729, 841725, 872729, 365456, 365456, 365456, 517521, 841725, 334239, 334239 ] },
//   v: { $in: [ 872729, 841725, 872729, 365456, 365456, 365456, 517521, 841725, 334239, 334239 ] },
//   j: { $in: [ 334239, 872729, 365456, 517521, 365456, 365456, 334239, 872729, 334239, 841725 ] },
//   j: { $in: [ 334239, 872729, 365456, 517521, 365456, 365456, 334239, 872729, 334239, 841725 ] },
//   i: { $in: [ 841725, 517521, 517521, 365456, 334239, 872729, 872729, 872729, 872729, 841725 ] },
//   i: { $in: [ 841725, 517521, 517521, 365456, 334239, 872729, 872729, 872729, 872729, 841725 ] },
//   n: { $in: [ 872729, 365456, 872729, 517521, 365456, 517521, 841725, 872729, 334239, 365456 ] },
//   n: { $in: [ 872729, 365456, 872729, 517521, 365456, 517521, 841725, 872729, 334239, 365456 ] }
//   } }
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithInPredicates,
            /*numDistinctMatchStatements*/ 8,
            /*numOfDistinctValues*/ 5,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 0,                           // plays no role here.
            /*dottedPathType*/ DottedPathType::kSimple,  // plays no role here.
            /*dottedPathLength*/ 0,                      // plays no role here.
            /*sizeOfInArray*/ 10,
            /*numOfNestings*/ 0});

// _id queries.
// { find: "exampleCol", $db: "foo", filter:
// { _id: 365456, _id: 365456, _id: 517521, _id: 517521, _id: 334239, _id: 334239,
//   _id: 841725, _id: 841725, _id: 841725, _id: 841725, _id: 365456, _id: 365456,
//   _id: 334239, _id: 334239, _id: 517521, _id: 517521, _id: 334239, _id: 334239,
//   _id: 517521, _id: 517521 }}
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithIdPredicates,
            /*numDistinctMatchStatements*/ 10,
            /*numOfDistinctValues*/ 4,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 0,                           // plays no role here.
            /*dottedPathType*/ DottedPathType::kSimple,  // plays no role here.
            /*dottedPathLength*/ 0,                      // plays no role here.
            /*sizeOfInArray*/ 0,                         // plays no role here.
            /*numOfNestings*/ 0});                       // plays no role here.


// nested $OR queries.
// { find: "exampleCol", $db: "foo", filter:
// { $or: [
//     { $or: [
//         { $or: [
//             { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//               v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//               n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 },
//             { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//               v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//               n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 }
//             ] },
//         { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//           v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//           n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 }
//         ] },
//     { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//       v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//       n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 }
//     ] } }
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithNestedOrPredicates,
            /*numDistinctMatchStatements*/ 10,
            /*numOfDistinctValues*/ 4,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 0,                           // plays no role here.
            /*dottedPathType*/ DottedPathType::kSimple,  // plays no role here.
            /*dottedPathLength*/ 0,                      // plays no role here.
            /*sizeOfInArray*/ 0,                         // plays no role here.
            /*numOfNestings*/ 3});                       // needs to be > 0

// nested $AND queries.
// { find: "exampleCol", $db: "foo", filter:
// { $and: [
//     { $and: [
//         { $and: [
//             { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//               v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//               n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 },
//             { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//               v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//               n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 } ] },
//         { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//           v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//           n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 } ] },
//     { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//       v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//       n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 }
//     ] } }
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithNestedAndPredicates,
            /*numDistinctMatchStatements*/ 10,
            /*numOfDistinctValues*/ 4,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 0,                           // plays no role here.
            /*dottedPathType*/ DottedPathType::kSimple,  // plays no role here.
            /*dottedPathLength*/ 0,                      // plays no role here.
            /*sizeOfInArray*/ 0,                         // plays no role here.
            /*numOfNestings*/ 3});                       // needs to be > 0

//
// { find: "exampleCol", $db: "foo", filter:
// { $and: [
//     { $or: [
//         { $and: [
//             { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//               v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//               n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 },
//             { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//               v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//               n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 } ] },
//         { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//           v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//           n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 } ] },
//     { j: 365456, j: 365456, n: 517521, n: 517521, i: 334239, i: 334239, v: 841725,
//       v: 841725, v: 841725, v: 841725, j: 365456, j: 365456, i: 334239, i: 334239,
//       n: 517521, n: 517521, i: 334239, i: 334239, n: 517521, n: 517521 }
//     ] } }
BENCHMARK(BM_CreateCanonicalQuery)
    ->Args({QueryType::kFindWithNestedAndOrPredicates,
            /*numDistinctMatchStatements*/ 10,
            /*numOfDistinctValues*/ 4,
            /*numOfDistinctFields*/ 4,
            /*numOfRepetitionsPerStatement*/ 2,
            /*dottedPaths*/ 0,                           // plays no role here.
            /*dottedPathType*/ DottedPathType::kSimple,  // plays no role here.
            /*dottedPathLength*/ 0,                      // plays no role here.
            /*sizeOfInArray*/ 0,                         // plays no role here.
            /*numOfNestings*/ 3});                       // needs to be > 0

}  // namespace mongo
