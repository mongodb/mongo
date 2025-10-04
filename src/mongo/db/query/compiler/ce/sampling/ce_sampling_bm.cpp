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

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/multiple_collection_accessor.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::ce {

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
          /*seed*/ seed_value3_low)}},
    {2,
     {CollectionFieldConfiguration({/*fieldName*/ "a",
                                    /*fieldPosition*/ 0,
                                    /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                                    /*ndv*/ 500,
                                    /*dataDistribution*/ stats::DistrType::kUniform,
                                    /*seed*/ seed_value1_low})}},
    {3,
     {CollectionFieldConfiguration(
         /*fieldName*/ "a",
         /*fieldPosition*/ 5,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 500,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1_low)}},
    {4,
     {CollectionFieldConfiguration(
         /*fieldName*/ "f0",
         /*fieldPosition*/ 0,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 1000,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1_low)}},
    {5,
     {CollectionFieldConfiguration(
         /*fieldName*/ "f0",
         /*fieldPosition*/ 20,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 1000,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1_low)}},
    {6,
     {CollectionFieldConfiguration(
         /*fieldName*/ "f0",
         /*fieldPosition*/ 50,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 1000,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1_low)}},
    {7,
     {CollectionFieldConfiguration(
         /*fieldName*/ "f0",
         /*fieldPosition*/ 99,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 1000,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1_low)}},
    {8,
     {CollectionFieldConfiguration(
          /*fieldName*/ "f0",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f1",
          /*fieldPosition*/ 1,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f2",
          /*fieldPosition*/ 2,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f3",
          /*fieldPosition*/ 3,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f4",
          /*fieldPosition*/ 4,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f5",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f6",
          /*fieldPosition*/ 6,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f7",
          /*fieldPosition*/ 7,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f8",
          /*fieldPosition*/ 8,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f9",
          /*fieldPosition*/ 9,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low)}},
    {9,
     {CollectionFieldConfiguration(
          /*fieldName*/ "f0",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f1",
          /*fieldPosition*/ 21,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f2",
          /*fieldPosition*/ 22,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f3",
          /*fieldPosition*/ 23,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f4",
          /*fieldPosition*/ 24,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f5",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f6",
          /*fieldPosition*/ 26,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f7",
          /*fieldPosition*/ 27,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f8",
          /*fieldPosition*/ 28,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f9",
          /*fieldPosition*/ 29,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low)}},
    {10,
     {CollectionFieldConfiguration(
          /*fieldName*/ "f0",
          /*fieldPosition*/ 50,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f1",
          /*fieldPosition*/ 51,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f2",
          /*fieldPosition*/ 52,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f3",
          /*fieldPosition*/ 53,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f4",
          /*fieldPosition*/ 54,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f5",
          /*fieldPosition*/ 55,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f6",
          /*fieldPosition*/ 56,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f7",
          /*fieldPosition*/ 57,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f8",
          /*fieldPosition*/ 58,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f9",
          /*fieldPosition*/ 59,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low)}},
    {11,
     {CollectionFieldConfiguration(
          /*fieldName*/ "f0",
          /*fieldPosition*/ 90,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f1",
          /*fieldPosition*/ 91,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f2",
          /*fieldPosition*/ 92,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f3",
          /*fieldPosition*/ 93,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f4",
          /*fieldPosition*/ 94,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f5",
          /*fieldPosition*/ 95,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f6",
          /*fieldPosition*/ 96,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f7",
          /*fieldPosition*/ 97,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f8",
          /*fieldPosition*/ 98,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f9",
          /*fieldPosition*/ 99,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low)}},
    {12,
     {CollectionFieldConfiguration(
          /*fieldName*/ "f0",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f1",
          /*fieldPosition*/ 10,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f2",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f3",
          /*fieldPosition*/ 30,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f4",
          /*fieldPosition*/ 40,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f5",
          /*fieldPosition*/ 50,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f6",
          /*fieldPosition*/ 60,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f7",
          /*fieldPosition*/ 70,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f8",
          /*fieldPosition*/ 80,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f9",
          /*fieldPosition*/ 90,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low)}},
    {13,
     {CollectionFieldConfiguration(
          /*fieldName*/ "f0",
          /*fieldPosition*/ 1,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f1",
          /*fieldPosition*/ 2,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f2",
          /*fieldPosition*/ 3,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f3",
          /*fieldPosition*/ 4,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f4",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f5",
          /*fieldPosition*/ 95,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f6",
          /*fieldPosition*/ 96,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f7",
          /*fieldPosition*/ 97,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f8",
          /*fieldPosition*/ 98,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f9",
          /*fieldPosition*/ 99,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low)}},
    {14,
     {CollectionFieldConfiguration(
          /*fieldName*/ "f0",
          /*fieldPosition*/ 1,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f1",
          /*fieldPosition*/ 91,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f2",
          /*fieldPosition*/ 92,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f3",
          /*fieldPosition*/ 93,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f4",
          /*fieldPosition*/ 94,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f5",
          /*fieldPosition*/ 95,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f6",
          /*fieldPosition*/ 96,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f7",
          /*fieldPosition*/ 97,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f8",
          /*fieldPosition*/ 98,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low),
      CollectionFieldConfiguration(
          /*fieldName*/ "f9",
          /*fieldPosition*/ 99,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1_low)}},
    {15,
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
          /*fieldName*/ "d",
          /*fieldPosition*/ 25,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 1000,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value4_low)}},
    {16,
     {CollectionFieldConfiguration(
         /*fieldName*/ "a",
         /*fieldPosition*/ 50,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 500,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1_low)}},
    {17,
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
          /*seed*/ seed_value2_low)}}};

/**
 * This map defines the set of attributes queries will evaluate predicates on.
 * The seeds represent the seeds for the lower and upper values for a range.
 * For a point query only the first seed is used whereas for range query both seeds are relevant.
 * For range queries, ensure that the two seed values differ, otherwise the queries become in
 * essence point queries.
 */
auto queryConfig1 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "a",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ 500,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value1_low, seed_value1_high})},
                       /*queryTypes*/ {kRange}));

auto queryConfig2 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            {seed_value2_low, seed_value2_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_high})},
                       /*queryTypes*/ {kPoint, kPoint}));

auto queryConfig3 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            {seed_value1_low, seed_value1_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_high})},
                       /*queryTypes*/ {kPoint, kPoint}));

auto queryConfig4 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "a",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ 500,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value1_low, seed_value1_high})},
                       /*queryTypes*/ {kPoint}));

// Point Queries with various field positions.
auto queryConfig5 = WorkloadConfiguration(
    /* numberOfQueries */ 1,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::NumberInt64,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1_low, seed_value1_high})},
                       /* queryTypes */ {kPoint}));

auto queryConfig6 = WorkloadConfiguration(
    /* numberOfQueries */ 1,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f5",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f6",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f7",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f8",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f9",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
        },
        /* queryTypes */
        {kPoint, kPoint, kPoint, kPoint, kPoint, kPoint, kPoint, kPoint, kPoint, kPoint}));

// Range Queries with various field positions.
auto queryConfig7 = WorkloadConfiguration(
    /* numberOfQueries */ 1,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::NumberInt64,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1_low, seed_value1_high})},
                       /* queryTypes */ {kRange}));

auto queryConfig8 = WorkloadConfiguration(
    /* numberOfQueries */ 1,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f5",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f6",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f7",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f8",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
            DataFieldDefinition(
                /* fieldName*/ "f9",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1_low, seed_value1_high}),
        },
        /* queryTypes */
        {kRange, kRange, kRange, kRange, kRange, kRange, kRange, kRange, kRange, kRange}));

int numberOfGeneratedQueries = 7;

/**
 * The following definitions represent the workload configuration.
 * The number of queries defines the number or queries generated not the actual number of queries
 * (that is defined by number of iterations)
 * The NDV value defines the number of distinct values will be created for predicates. As long as
 * this number is larger than numberOfQueries, all queries will be unique.
 * This set of queries aims to benchmark the following dimensions
 * 1. querying on fields in varying positions in documents e.g., 1A vs 1D
 * 2. querying of varying number of fields 1, 2, 3, 4
 * 3. point and range queries
 */
auto pointQueryConfig1A = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "a",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ numberOfGeneratedQueries,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value1_low, seed_value1_low})},
                       /*queryTypes*/ {kPoint}));

auto rangeQueryConfig1A = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "a",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ numberOfGeneratedQueries,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value1_low, seed_value1_high})},
                       /*queryTypes*/ {kRange}));

auto pointQueryConfig1D = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "d",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ numberOfGeneratedQueries,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value4_low, seed_value4_low})},
                       /*queryTypes*/ {kPoint}));

auto rangeQueryConfig1D = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "d",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ numberOfGeneratedQueries,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value4_low, seed_value4_high})},
                       /*queryTypes*/ {kRange}));

auto pointQueryConfig2 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low})},
                       /*queryTypes*/ {kPoint, kPoint}));

auto rangeQueryConfig2 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_high})},
                       /*queryTypes*/ {kRange, kRange}));

auto pointQueryConfig3 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_low})},
                       /*queryTypes*/ {kPoint, kPoint, kPoint}));

auto rangeQueryConfig3 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_high})},
                       /*queryTypes*/ {kRange, kRange, kRange}));

auto pointQueryConfig4 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_low}),
                        DataFieldDefinition(
                            /*fieldName*/ "d",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value4_low, seed_value4_low})},
                       /*queryTypes*/ {kPoint, kPoint, kPoint, kPoint}));

auto rangeQueryConfig4 = WorkloadConfiguration(
    /*numberOfQueries*/ numberOfGeneratedQueries,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1_low, seed_value1_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value2_low, seed_value2_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value3_low, seed_value3_high}),
                        DataFieldDefinition(
                            /*fieldName*/ "d",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ numberOfGeneratedQueries,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value4_low, seed_value4_high})},
                       /*queryTypes*/ {kRange, kRange, kRange, kRange}));

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
    {1, queryConfig1},
    {2, queryConfig2},
    {3, queryConfig3},
    {4, queryConfig4},
    {5, queryConfig5},
    {6, queryConfig6},
    {7, queryConfig7},
    {8, queryConfig8},
    {9, pointQueryConfig1A},
    {10, rangeQueryConfig1A},
    {11, pointQueryConfig1D},
    {12, rangeQueryConfig1D},
    {13, pointQueryConfig2},
    {14, rangeQueryConfig2},
    {15, pointQueryConfig3},
    {16, rangeQueryConfig3},
    {17, pointQueryConfig4},
    {18, rangeQueryConfig4},
};

void printResults(size_t dataSize,
                  size_t dataFieldsConfiguration,
                  SampleSizeDef errorConfig,
                  size_t sampleSize,
                  size_t actualSampleSize,
                  size_t samplingStyleConfig,
                  double ms_doubleCREATESAMPLE,
                  size_t queryFieldConfig,
                  std::vector<double>& estimationTimes) {
    BSONObjBuilder totalBuilder;
    totalBuilder << "DataSize" << (int)dataSize;
    totalBuilder << "DataFieldConfiguration" << (int)dataFieldsConfiguration;
    totalBuilder << "ErrorConfig" << (int)errorConfig;
    totalBuilder << "SampleSize" << (int)sampleSize;
    totalBuilder << "ActualSampleSize" << (int)actualSampleSize;
    totalBuilder << "SamplingStyle" << (int)samplingStyleConfig;
    totalBuilder << "SampleCreationMS" << ms_doubleCREATESAMPLE;
    totalBuilder << "QueryConfig" << (int)queryFieldConfig;
    totalBuilder << "EstimationMS" << estimationTimes;
    std::cout << totalBuilder.obj() << std::endl;
}

void BM_RunAllConfigs(benchmark::State& state) {
    // Define all the settings to run.
    std::vector<size_t> dataSizes = {10000, 100000, 500000};
    std::vector<size_t> dataFieldsConfiguration = {
        1,  // 2, 3, 15, 16, 17
    };
    std::vector<size_t> queryFieldConfig = {
        9  //, 10, 11, 12, 13, 14, 15, 16, 17, 18
    };
    std::vector<SampleSizeDef> errorConfigs = {
        SampleSizeDef::ErrorSetting1, SampleSizeDef::ErrorSetting2, SampleSizeDef::ErrorSetting5};
    std::vector<int> samplingStyleConfig = {-1, 10, 20};

    for (size_t dataSize_idx = 0; dataSize_idx < dataSizes.size(); dataSize_idx++) {
        for (size_t dataFieldsConfiguration_idx = 0;
             dataFieldsConfiguration_idx < dataFieldsConfiguration.size();
             dataFieldsConfiguration_idx++) {
            // Translate the fields and positions configurations based on the defined map.
            auto fieldsConfig =
                collectionFieldConfigurations[dataFieldsConfiguration[dataFieldsConfiguration_idx]];

            DataConfiguration dataConfig(
                /*dataSize*/ dataSizes[dataSize_idx],
                /*dataFieldConfig*/ fieldsConfig);

            // Generate data and populate source collection
            SamplingEstimatorTest samplingEstimatorTest;
            initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

            for (size_t errorConfigs_idx = 0; errorConfigs_idx < errorConfigs.size();
                 errorConfigs_idx++) {
                // Translate the sample size definition to corresponding sample size.
                auto sampleSize = translateSampleDefToActualSampleSize(
                    /*sampleSizeDef*/ errorConfigs[errorConfigs_idx]);

                for (size_t samplingStyleConfig_idx = 0;
                     samplingStyleConfig_idx < samplingStyleConfig.size();
                     samplingStyleConfig_idx++) {
                    // Translate the number of chunks variable to both number of chunks and sampling
                    // algo. This benchmark given as input numOfChunks <= 0 will use kRandom.
                    auto samplingStyle = iniitalizeSamplingAlgoBasedOnChunks(
                        /*samplingAlgo-numOfChunks*/ samplingStyleConfig[samplingStyleConfig_idx]);

                    // Initialize collection accessor
                    auto opCtx = samplingEstimatorTest.getOperationContext();
                    auto acquisition = acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                                samplingEstimatorTest._kTestNss,
                                                                AcquisitionPrerequisites::kWrite),
                        LockMode::MODE_IX);
                    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

                    // CREATE SAMPLE ESTIMATOR
                    auto createSample_start = high_resolution_clock::now();
                    // Create sample from the provided collection
                    SamplingEstimatorImpl samplingEstimator(
                        samplingEstimatorTest.getOperationContext(),
                        collection,
                        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                        sampleSize,
                        samplingStyle.first,
                        samplingStyle.second,
                        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
                    samplingEstimator.generateSample(ce::NoProjection{});

                    auto createSample_end = high_resolution_clock::now();
                    duration<double, std::milli> ms_doubleCREATESAMPLE =
                        createSample_end - createSample_start;

                    // Run query configs.
                    for (size_t queryFieldConfig_idx = 0;
                         queryFieldConfig_idx < queryFieldConfig.size();
                         queryFieldConfig_idx++) {
                        // Setup query types.
                        WorkloadConfiguration& workloadConfig =
                            queryFieldsConfigurations.at(queryFieldConfig[queryFieldConfig_idx]);

                        // Generate queries.
                        std::vector<std::unique_ptr<MatchExpression>> allMatchExpressionQueries =
                            generateMatchExpressionsBasedOnWorkloadConfig(workloadConfig);

                        std::vector<double> estimationTimes;
                        for (int iteration = 0; iteration < numberOfGeneratedQueries; iteration++) {
                            // RUN QUERY
                            auto estimate_start = high_resolution_clock::now();
                            samplingEstimator.estimateCardinality(
                                allMatchExpressionQueries[iteration].get());
                            auto estimate_end = high_resolution_clock::now();
                            duration<double, std::milli> ms_doubleESTIMATE =
                                estimate_end - estimate_start;
                            estimationTimes.push_back(ms_doubleESTIMATE.count());
                        }

                        printResults(dataSizes[dataSize_idx],
                                     dataFieldsConfiguration[dataFieldsConfiguration_idx],
                                     errorConfigs[errorConfigs_idx],
                                     sampleSize,
                                     samplingEstimator.getSampleSize(),
                                     samplingStyleConfig[samplingStyleConfig_idx],
                                     ms_doubleCREATESAMPLE.count(),
                                     queryFieldConfig[queryFieldConfig_idx],
                                     estimationTimes);
                    }
                }
            }
            samplingEstimatorTest.tearDown();
        }
    }

    for (auto _ : state) {
        std::cout << "Done Benchmark" << std::endl;
    }
}

void BM_CreateSample(benchmark::State& state) {
    // Translate the fields and positions configurations based on the defined map.
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];

    DataConfiguration dataConfig(
        /*dataSize*/ state.range(0),
        /*dataFieldConfig*/ fieldsConfig);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef*/ static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto sampling =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    auto opCtx = samplingEstimatorTest.getOperationContext();
    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, samplingEstimatorTest._kTestNss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

    for (auto _ : state) {
        // Create sample from the provided collection
        SamplingEstimatorImpl samplingEstimator(
            samplingEstimatorTest.getOperationContext(),
            collection,
            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
            sampleSize,
            sampling.first,
            sampling.second,
            SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
        samplingEstimator.generateSample(ce::NoProjection{});
    }
}

void BM_RunCardinalityEstimationOnSample(benchmark::State& state) {
    // Translate the fields and positions configurations based on the defined map.
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];

    DataConfiguration dataConfig(
        /*dataSize*/ state.range(0),
        /*dataFieldConfig*/ fieldsConfig);

    // Setup query types.
    WorkloadConfiguration& workloadConfig = queryFieldsConfigurations.at(state.range(5));

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    auto opCtx = samplingEstimatorTest.getOperationContext();
    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, samplingEstimatorTest._kTestNss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef*/ static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto sampling =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Create sample from the provided collection
    SamplingEstimatorImpl samplingEstimator(
        samplingEstimatorTest.getOperationContext(),
        collection,
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        sampleSize,
        sampling.first,
        sampling.second,
        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
    samplingEstimator.generateSample(ce::NoProjection{});

    // Generate queries.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals =
        generateMultiFieldIntervals(workloadConfig);

    std::vector<std::unique_ptr<MatchExpression>> allMatchExpressionQueries =
        createQueryMatchExpressionOnMultipleFields(workloadConfig, queryFieldsIntervals);

    size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            samplingEstimator.estimateCardinality(allMatchExpressionQueries[i].get()));
        i = (i + 1) % allMatchExpressionQueries.size();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_RunCardinalityEstimationOnSampleWithProjection(benchmark::State& state) {
    // Translate the fields and positions configurations based on the defined map.
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];

    DataConfiguration dataConfig(
        /*dataSize*/ state.range(0),
        /*dataFieldConfig*/ fieldsConfig);

    // Setup query types.
    WorkloadConfiguration& workloadConfig = queryFieldsConfigurations.at(state.range(5));

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    auto opCtx = samplingEstimatorTest.getOperationContext();
    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, samplingEstimatorTest._kTestNss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef*/ static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto sampling =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    StringSet queryFieldNames;
    for (const auto& queryField : workloadConfig.queryConfig.queryFields) {
        queryFieldNames.insert(queryField.fieldName);
    }

    // Create sample from the provided collection
    SamplingEstimatorImpl samplingEstimator(
        samplingEstimatorTest.getOperationContext(),
        collection,
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        sampleSize,
        sampling.first,
        sampling.second,
        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
    samplingEstimator.generateSample(queryFieldNames);

    // Generate queries.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals =
        generateMultiFieldIntervals(workloadConfig);

    std::vector<std::unique_ptr<MatchExpression>> allMatchExpressionQueries =
        createQueryMatchExpressionOnMultipleFields(workloadConfig, queryFieldsIntervals);

    size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            samplingEstimator.estimateCardinality(allMatchExpressionQueries[i].get()));
        i = (i + 1) % allMatchExpressionQueries.size();
    }
    state.SetItemsProcessed(state.iterations());
}

/**
 * Evaluate the performance of preparing a sample and estimating CE over that sample. This
 * invocation will vary the number documents and number of fields in the base collection, the sample
 * size, as well as the type of queries (point and range).
 */
BENCHMARK(BM_RunAllConfigs)->Iterations(1)->Unit(benchmark::kMillisecond);

/**
 * Evaluate the performance of preparing the sampling CE estimator which mainly concentrates on
 * creating samples using a variety of Sampling strategies. This invocation will vary the number
 * documents and number of fields in the base collection as well as the sample size.
 */
BENCHMARK(BM_CreateSample)
    ->ArgNames({"dataSize", "dataFieldsConfiguration", "sampleSizeDef", "samplingAlgo-numChunks"})
    ->ArgsProduct({/*dataSize*/ {100},
                   /*dataFieldsConfiguration*/ {1},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1}})
    ->Unit(benchmark::kMillisecond);

/**
 * Evaluate the performance of estimating CE using an already populated sample. The
 * estimation mainly concentrates on processing the already existing sample and
 * extrapolating the cardinality results. This invocation will vary the number documents and
 * number of fields in the base collection, the type of queries (point and range), as well
 * as the sample size.
 */
BENCHMARK(BM_RunCardinalityEstimationOnSample)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "numberOfQueries",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {100},
                   /*dataFieldsConfiguration*/ {1},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*numberOfQueries*/ {50},
                   /*queryFieldConfig*/ {1}})
    ->Unit(benchmark::kMillisecond);


/* Sample Field Projection Benchmarks */
// Single Field Queries.
BENCHMARK(BM_RunCardinalityEstimationOnSample)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "numberOfQueries",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {100000},
                   /*collectionFieldConfiguration*/ {4, 5, 6, 7},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*numberOfQueries*/ {1},
                   /*queryFieldConfig*/ {5, 7}})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_RunCardinalityEstimationOnSampleWithProjection)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "numberOfQueries",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {100000},
                   /*collectionFieldConfiguration*/ {4, 5, 6, 7},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*numberOfQueries*/ {1},
                   /*queryFieldConfig*/ {5, 7}})
    ->Unit(benchmark::kMillisecond);

// 10 field queries.
BENCHMARK(BM_RunCardinalityEstimationOnSample)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "numberOfQueries",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {100000},
                   /*collectionFieldConfiguration*/ {8, 9, 10, 11, 12, 13, 14},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*numberOfQueries*/ {1},
                   /*queryFieldConfig*/ {6, 8}})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.

BENCHMARK(BM_RunCardinalityEstimationOnSampleWithProjection)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "numberOfQueries",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {100000},
                   /*collectionFieldConfiguration*/ {4, 5, 6, 7},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*numberOfQueries*/ {1},
                   /*queryFieldConfig*/ {6, 8}})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.
}  // namespace mongo::ce
