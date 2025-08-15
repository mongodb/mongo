/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/plan_ranking_utils.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/transport/service_entry_point.h"

#include <benchmark/benchmark.h>

using namespace mongo::ce;
using namespace mongo;

using std::chrono::duration;
using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;

// Defining a constant seed value for data generation, the use of this ensure that point queries
// will match against at least one generated document in the dataset.
const size_t seed_value1_low = 1724178;
const size_t seed_value1_high = 6123542;

const size_t seed_value2_low = 8713211;
const size_t seed_value2_high = 3241231;

const size_t seed_value3_low = 9247231;
const size_t seed_value3_high = 63523840;

const size_t seed_value4_low = 19984748;
const size_t seed_value4_high = 985235;

const size_t seed_value5_low = 584573;
const size_t seed_value5_high = 37593;

/**
 * This map defines a set of configurations for collection generation.
 * The keys of the map are used as part of the benchmark state inputs to create a variety of base
 * collection to test against.
 * Each configuration requires a vector of CollectionFieldConfigurations which represent the set of
 * "user defined" fields. Each field requires configuring its name, position in the collection as
 * well as type and distribution information.
 * The collection will contain as many fields as the maximum position of the user defined fields.
 * The remaining in-between fields are copies of the user defined fields with names with suffix an
 * underscore and a number.
 * When defining the fields in the collection, order the fields in increasing position order.
 */
std::map<int, std::vector<CollectionFieldConfiguration>> collectionFieldConfigurations = {
    {1,
     {CollectionFieldConfiguration(
          /*fieldName*/ "a",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "b",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value2_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "c",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value3_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "e",
          /*fieldPosition*/ 22,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value5_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "d",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value4_low)}},
    {2,
     {CollectionFieldConfiguration(
          /*fieldName*/ "a",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "b",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value2_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "c",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value3_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "e",
          /*fieldPosition*/ 22,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value5_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "d",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value4_low)}},
    {3,
     {CollectionFieldConfiguration(
          /*fieldName*/ "a",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 250,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "b",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 250,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value2_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "c",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 250,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value3_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "e",
          /*fieldPosition*/ 22,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 250,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value5_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "d",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 250,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value4_low)}},
    {4,
     {CollectionFieldConfiguration(
          /*fieldName*/ "a",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 100,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "b",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 100,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value2_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "c",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 100,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value3_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "e",
          /*fieldPosition*/ 22,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 100,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value5_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "d",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 100,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value4_low)}},
    {5,
     {CollectionFieldConfiguration(
          /*fieldName*/ "a",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 50,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "b",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 50,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value2_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "c",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 50,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value3_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "e",
          /*fieldPosition*/ 22,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 50,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value5_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "d",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 50,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value4_low)}},
    {6,
     {CollectionFieldConfiguration(
          /*fieldName*/ "a",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 10,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "b",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 10,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value2_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "c",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 10,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value3_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "e",
          /*fieldPosition*/ 22,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 10,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value5_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "d",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 10,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value4_low)}}

};

int numberOfIterations = 7;

auto pointQueryConfig1A = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,  // This defines the number or queries generated not the
                                             // actual number of queries (that is defined by number
                                             // of iterations)
    QueryConfiguration(
        {DataFieldDefinition(
            /*fieldName*/ "a",
            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
            /*ndv*/ numberOfIterations,  // as long as this number is larger than numberOfQueries,
                                         // all queries will be unique.
            /*dataDistribution*/ stats::DistrType::kUniform,
            /*seed*/ {seed_value1_low, seed_value1_low})},
        /*queryTypes*/ {kPoint}));

auto rangeQueryConfig1A = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "a",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ numberOfIterations,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value1_low, seed_value1_high})},
                       /*queryTypes*/ {kRange}));

auto pointQueryConfig1A_E = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,  // This defines the number or queries generated not the
                                             // actual number of queries (that is defined by number
                                             // of iterations)
    QueryConfiguration(
        {DataFieldDefinition(
             /*fieldName*/ "a",
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ numberOfIterations,  // as long as this number is larger than numberOfQueries,
                                          // all queries will be unique.
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ {seed_value1_low, seed_value1_low}),
         DataFieldDefinition(
             /*fieldName*/ "e",
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ numberOfIterations,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ {seed_value5_low, seed_value5_low})},
        /*queryTypes*/ {kPoint, kPoint}));

auto pointQueryConfig1D = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,  // This defines the number or queries generated not the
                                             // actual number of queries (that is defined by number
                                             // of iterations)
    QueryConfiguration(
        {DataFieldDefinition(
            /*fieldName*/ "d",
            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
            /*ndv*/ numberOfIterations,  // as long as this number is larger than numberOfQueries,
                                         // all queries will be unique.
            /*dataDistribution*/ stats::DistrType::kUniform,
            /*seed*/ {seed_value4_low, seed_value4_low})},
        /*queryTypes*/ {kPoint}));

auto rangeQueryConfig1D = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "d",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ numberOfIterations,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value4_low, seed_value4_high})},
                       /*queryTypes*/ {kRange}));

auto pointQueryConfig1D_E = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,  // This defines the number or queries generated not the
                                             // actual number of queries (that is defined by number
                                             // of iterations)
    QueryConfiguration(
        {DataFieldDefinition(
             /*fieldName*/ "d",
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ numberOfIterations,  // as long as this number is larger than numberOfQueries,
                                          // all queries will be unique.
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ {seed_value4_low, seed_value4_low}),
         DataFieldDefinition(
             /*fieldName*/ "e",
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ numberOfIterations,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ {seed_value5_low, seed_value5_low})},
        /*queryTypes*/ {kPoint, kPoint}));

auto pointQueryConfig2 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low})},
                       /*queryTypes*/ {kPoint, kPoint}));

auto rangeQueryConfig2 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_high})},
                       /*queryTypes*/ {kRange, kRange}));

auto pointQueryConfig2_E = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "e",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value5_low, seed_value5_low})},
                       /*queryTypes*/ {kPoint, kPoint, kPoint}));

auto pointQueryConfig3 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_low})},
                       /*queryTypes*/ {kPoint, kPoint, kPoint}));

auto rangeQueryConfig3 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_high})},
                       /*queryTypes*/ {kRange, kRange, kRange}));

auto pointQueryConfig3_E = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "e",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value5_low, seed_value5_low})},
                       /*queryTypes*/ {kPoint, kPoint, kPoint, kPoint}));

auto pointQueryConfig4 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "d",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value4_low, seed_value4_low})},
                       /*queryTypes*/ {kPoint, kPoint, kPoint, kPoint}));

auto rangeQueryConfig4 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "d",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value4_low, seed_value4_high})},
                       /*queryTypes*/ {kRange, kRange, kRange, kRange}));

auto pointQueryConfig4_E = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfIterations,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "d",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value4_low, seed_value4_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "e",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfIterations,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value5_low, seed_value5_low})},
                       /*queryTypes*/ {kPoint, kPoint, kPoint, kPoint, kPoint}));

/**
 * This map defines a set of configurations for workload generation.
 * The keys of the map are used as part of the benchmark state inputs to create a variety of the
 * workload to run against the defined collection. Each configuration requires a vector of
 * QueryConfiguration represents the set of fields to be queried against. Each field
 * requires configuring its name as well as type and distribution information. The implementation
 * currently supports only conjunction (AND) correlation between fields.
 * IMPORTANT: To ensure that point queries will have matches, ensure that the seeds provided for
 * query value generation to be identical with the generation of the dataset.
 */
std::map<int, WorkloadConfiguration&> queryFieldsConfigurations = {
    {1, pointQueryConfig1A},
    {2, rangeQueryConfig1A},
    {3, pointQueryConfig1D},
    {4, rangeQueryConfig1D},
    {5, pointQueryConfig2},
    {6, rangeQueryConfig2},
    {7, pointQueryConfig3},
    {8, rangeQueryConfig3},
    {9, pointQueryConfig4},
    {10, rangeQueryConfig4},
    {11, pointQueryConfig1A_E},
    {12, pointQueryConfig1D_E},
    {13, pointQueryConfig2_E},
    {14, pointQueryConfig3_E},
    {15, pointQueryConfig4_E},
};

BSONObj printPerformanceResults(size_t dataSize,
                                size_t dataFieldsConfiguration,
                                size_t queryFieldConfig,
                                PlanRankingExecutionStatistics executionStats,
                                std::string indexCombinationString) {
    BSONObjBuilder totalBuilder;

    BSONObjBuilder builderCollectedInfo;
    builderCollectedInfo << "DataSize" << (int)dataSize;
    builderCollectedInfo << "DataFieldConfiguration" << (int)dataFieldsConfiguration;
    builderCollectedInfo << "IndexCombination" << indexCombinationString;
    builderCollectedInfo << "QueryConfig" << (int)queryFieldConfig;
    totalBuilder << "CollectedInfo" << builderCollectedInfo.obj();

    totalBuilder << "Selectivities" << executionStats.selectivities;
    totalBuilder << "MultiPlannerExecTimes" << executionStats.multiplannerExecTimes;

    for (const auto& specificSamplingStyleCBR : executionStats.cbrExecTimes) {
        BSONObjBuilder cbrExecTimesBuilder;
        for (const auto& samplingAlgoAndRuntimes : specificSamplingStyleCBR.second) {
            cbrExecTimesBuilder << std::to_string(static_cast<int>(samplingAlgoAndRuntimes.first))
                                << samplingAlgoAndRuntimes.second;
        }
        std::string CBRExecConfigName =
            "CBRExecTimes_" + std::to_string(specificSamplingStyleCBR.first);
        totalBuilder << CBRExecConfigName << cbrExecTimesBuilder.obj();
    }

    BSONObjBuilder builderBare;
    for (const auto& samplingAlgoAndRuntimes : executionStats.bareCQEvalExecTimes) {
        builderBare << std::to_string(static_cast<int>(samplingAlgoAndRuntimes.first))
                    << samplingAlgoAndRuntimes.second;
    }
    totalBuilder << "BAREExecTimes" << builderBare.obj();

    BSONObjBuilder sampleSizesBuilder;
    for (const auto& samplingAlgoAndSampleSizes : executionStats.cbrSampleSizes) {
        sampleSizesBuilder << std::to_string(static_cast<int>(samplingAlgoAndSampleSizes.first))
                           << samplingAlgoAndSampleSizes.second;
    }
    totalBuilder << "SampleSizes" << sampleSizesBuilder.obj();

    return totalBuilder.obj();
}

/**
 * This function aims to streamline the benchmarking of query planning by computing the time to plan
 * using the MultiPlanner as well as CBR with various error configurations in one invocation.
 * The setting of the configurations is done at the beginning of the function populating the
 * vectors: dataSizes, dataFieldsConfigurations, queryFieldConfigs, errorConfigs, indexConfigs
 */
void BM_RunAllConfigs(benchmark::State& state) {
    // Define all the settings to run.
    std::vector<size_t> dataSizes = {
        10000,  //  100000, 500000
    };
    std::vector<size_t> dataFieldsConfigurations = {
        1,  //  2, 3, 4, 5, 6
    };
    std::vector<size_t> queryFieldConfigs = {
        1,
        //  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    std::vector<SampleSizeDef> errorConfigs = {
        SampleSizeDef::ErrorSetting1,
        // SampleSizeDef::ErrorSetting2,
        // SampleSizeDef::ErrorSetting5
    };
    std::vector<IndexCombinationTestSettings> indexConfigs = {
        IndexCombinationTestSettings::kNone,
        // IndexCombinationTestSettings::kSingleFieldIdx,
        // IndexCombinationTestSettings::kAllIdxes
    };
    std::vector<int> samplingStyleConfig = {/*random*/ -1, /*chunk 10*/ 10, /*chunk 20*/ 20};

    // Iterate over collection data sizes.
    for (size_t dataSize_idx = 0; dataSize_idx < dataSizes.size(); dataSize_idx++) {
        // Iterate over collection configurations (i.e., number of fields and distributions).
        for (size_t dataFieldsConfiguration_idx = 0;
             dataFieldsConfiguration_idx < dataFieldsConfigurations.size();
             dataFieldsConfiguration_idx++) {
            // Translate the fields and positions configurations based on the defined map.
            DataConfiguration dataConfig(
                /*dataSize*/ dataSizes[dataSize_idx],
                /*dataFieldConfig*/
                collectionFieldConfigurations
                    [dataFieldsConfigurations[dataFieldsConfiguration_idx]]);

            // Generate data and populate source collection
            std::vector<std::vector<stats::SBEValue>> allData;
            generateDataBasedOnConfig(dataConfig, allData);
            auto dataBSON = SamplingEstimatorTest::createDocumentsFromSBEValue(
                allData, dataConfig.collectionFieldsConfiguration);

            // Iterate over index configurations
            for (size_t indexConfig_idx = 0; indexConfig_idx < indexConfigs.size();
                 indexConfig_idx++) {
                SamplingEstimatorTest planRankingTest;
                planRankingTest.setUp();

                // Build any indexes.
                // Do not include "e" in the combinations (e should always be without an index to be
                // a residual predicate in the relevant queries)
                auto indexCombinations =
                    getIndexCombinations({"a", "b", "c", "d"}, indexConfigs[indexConfig_idx]);
                std::string indexCombinationString =
                    createIndexesAccordingToConfiguration(planRankingTest, indexCombinations);
                planRankingTest.insertDocuments(planRankingTest._kTestNss, dataBSON);

                for (size_t queryFieldConfig_idx = 0;
                     queryFieldConfig_idx < queryFieldConfigs.size();
                     queryFieldConfig_idx++) {
                    // Setup query types.
                    WorkloadConfiguration& workloadConfig =
                        queryFieldsConfigurations.at(queryFieldConfigs[queryFieldConfig_idx]);

                    // Generate queries.
                    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>>
                        queryFieldsIntervals = generateMultiFieldIntervals(workloadConfig);

                    std::vector<std::unique_ptr<MatchExpression>> allMatchExpressionQueries =
                        createQueryMatchExpressionOnMultipleFields(workloadConfig,
                                                                   queryFieldsIntervals);

                    // Create accumulator for all the execution stats.
                    PlanRankingExecutionStatistics execStats;

                    // Iterate over a number of distinct queries (each iteration executes a
                    // different query) as generated according to the input.
                    for (int iteration = 0; iteration < numberOfIterations; iteration++) {
                        auto cq = createCanonicalQueryFromMatchExpression(
                            planRankingTest, allMatchExpressionQueries[iteration]->clone());

                        // Calculate actual result cardinality.
                        int sel = evaluateMatchExpressionAgainstDataWithLimit(
                            allMatchExpressionQueries[iteration]->clone(), dataBSON);
                        execStats.selectivities.push_back(sel);

                        // Measure the time to pick best plan using the MultiPlanner.
                        std::unique_ptr<MultiPlanStage> _mps;
                        auto multiplanner_start = high_resolution_clock::now();
                        plan_ranking_tests::pickBestPlan(cq.get(),
                                                         *(planRankingTest.getOperationContext()),
                                                         cq->getExpCtx(),
                                                         _mps,
                                                         planRankingTest._kTestNss);
                        auto multiplanner_end = high_resolution_clock::now();
                        duration<double, std::milli> multiPlanner_ms =
                            multiplanner_end - multiplanner_start;
                        execStats.multiplannerExecTimes.push_back(multiPlanner_ms.count());

                        // Iterate over various error configurations.
                        for (size_t errorConfigs_idx = 0; errorConfigs_idx < errorConfigs.size();
                             errorConfigs_idx++) {
                            // Translate the sample size definition to corresponding sample size.
                            auto sampleSize = translateSampleDefToActualSampleSize(
                                /*sampleSizeDef*/ static_cast<SampleSizeDef>(
                                    errorConfigs[errorConfigs_idx]));

                            execStats.cbrSampleSizes[errorConfigs[errorConfigs_idx]] = sampleSize;

                            // Measure the bare time to evaluate the MatchExpression against a
                            // sample-sized portion of the collection.
                            auto bare_start = high_resolution_clock::now();
                            evaluateMatchExpressionAgainstDataWithLimit(
                                allMatchExpressionQueries[iteration]->clone(),
                                dataBSON,
                                sampleSize);
                            auto bare_end = high_resolution_clock::now();
                            duration<double, std::milli> bare_ms = bare_end - bare_start;
                            execStats.bareCQEvalExecTimes[errorConfigs[errorConfigs_idx]].push_back(
                                bare_ms.count());

                            // Iterate over sampling style configurations (i.e., Random/Chunk)
                            for (size_t samplingStyleConfig_idx = 0;
                                 samplingStyleConfig_idx < samplingStyleConfig.size();
                                 samplingStyleConfig_idx++) {

                                // Translate the number of chunks variable to both number of chunks
                                // and sampling algo. This benchmark given as input numOfChunks <= 0
                                // will use kRandom.
                                auto samplingStyle = iniitalizeSamplingAlgoBasedOnChunks(
                                    /*samplingAlgo*/ samplingStyleConfig[samplingStyleConfig_idx]);

                                std::vector<std::unique_ptr<QuerySolution>> bestCBRPlan;
                                auto cbr_start = high_resolution_clock::now();
                                plan_ranking_tests::bestCBRPlan(
                                    cq.get(),
                                    dataConfig.size,
                                    *(planRankingTest.getOperationContext()),
                                    sampleSize,
                                    bestCBRPlan,
                                    planRankingTest._kTestNss,
                                    samplingStyle.first,
                                    samplingStyle.second);
                                auto cbr_end = high_resolution_clock::now();

                                duration<double, std::milli> ms_doubleCBR = cbr_end - cbr_start;
                                execStats
                                    .cbrExecTimes[static_cast<int>(
                                        samplingStyleConfig[samplingStyleConfig_idx])]
                                                 [errorConfigs[errorConfigs_idx]]
                                    .push_back(ms_doubleCBR.count());
                            }
                        }
                    }

                    std::cout << printPerformanceResults(
                                     dataSizes[dataSize_idx],
                                     dataFieldsConfigurations[dataFieldsConfiguration_idx],
                                     queryFieldConfigs[queryFieldConfig_idx],
                                     execStats,
                                     indexCombinationString)
                              << std::endl;
                }
                planRankingTest.tearDown();
            }
        }
    }

    for (auto _ : state) {
        std::cout << "Done Benchmark" << std::endl;
    }
}

BENCHMARK(BM_RunAllConfigs)->Iterations(1)->Unit(benchmark::kMillisecond);
