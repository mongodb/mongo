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

#include "mongo/db/query/compiler/stats/stats_cache_loader.h"

#include "mongo/base/string_data.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/max_diff.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader_impl.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader_test_fixture.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo::stats {
namespace {

class StatsCacheLoaderTest : public StatsCacheLoaderTestFixture {
protected:
    void createStatsCollection(NamespaceString nss);
    StatsCacheLoaderImpl _statsCacheLoader;
};

void StatsCacheLoaderTest::createStatsCollection(NamespaceString nss) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    auto db = autoColl.ensureDbExists(opCtx);
    WriteUnitOfWork wuow(opCtx);
    ASSERT(db->createCollection(opCtx, nss));
    wuow.commit();
}

TEST_F(StatsCacheLoaderTest, VerifyStatsLoadsScalar) {
    // Initialize histogram buckets.
    constexpr double doubleCount = 15.0;
    constexpr double trueCount = 12.0;
    constexpr double falseCount = 16.0;
    constexpr double numDocs = doubleCount + trueCount + falseCount;
    std::vector<Bucket> buckets{
        Bucket{1.0, 0.0, 1.0, 0.0, 1.0},
        Bucket{2.0, 5.0, 8.0, 1.0, 3.0},
        Bucket{3.0, 4.0, 15.0, 2.0, 6.0},
    };

    // Initialize histogram bounds.
    auto [boundsTag, boundsVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard boundsGuard{boundsTag, boundsVal};
    auto bounds = sbe::value::getArrayView(boundsVal);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 1.0);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 2.0);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 3.0);

    // Create a scalar histogram.
    TypeCounts tc{
        {sbe::value::TypeTags::NumberDouble, doubleCount},
        {sbe::value::TypeTags::Boolean, trueCount + falseCount},
    };
    auto ceHist = CEHistogram::make(
        ScalarHistogram::make(*bounds, buckets), tc, numDocs, trueCount, falseCount);
    auto expectedSerialized = ceHist->serialize();

    // Serialize histogram into a stats path.
    std::string path = "somePath";
    constexpr double sampleRate = 1.0;
    auto serialized = stats::makeStatsPath(path, numDocs, sampleRate, ceHist);

    // Initalize stats collection.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "stats");
    std::string statsColl(StatsCacheLoader::kStatsPrefix + "." + nss.coll());
    NamespaceString statsNss =
        NamespaceString::createNamespaceString_forTest(nss.db_forTest(), statsColl);
    createStatsCollection(statsNss);

    // Write serialized stats path to collection.
    AutoGetCollection autoColl(operationContext(), statsNss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(serialized), nullptr));
        wuow.commit();
    }

    // Read stats path & verify values are consistent with what we expect.
    auto actualAH = _statsCacheLoader.getStats(operationContext(), std::make_pair(nss, path)).get();
    auto actualSerialized = actualAH->serialize();

    ASSERT_BSONOBJ_EQ(expectedSerialized, actualSerialized);
}

TEST_F(StatsCacheLoaderTest, VerifyStatsLoadsArray) {
    constexpr double numDocs = 13.0;

    auto nonEmptyArrayVal = sbe::value::makeNewArray().second;
    auto nonEmptyArray = sbe::value::getArrayView(nonEmptyArrayVal);
    nonEmptyArray->push_back(sbe::value::TypeTags::NumberDouble, 1.0);
    nonEmptyArray->push_back(sbe::value::TypeTags::NumberDouble, 2.0);
    nonEmptyArray->push_back(sbe::value::TypeTags::NumberDouble, 3.0);

    auto emptyArray1Val = sbe::value::makeNewArray().second;
    auto emptyArray2Val = sbe::value::makeNewArray().second;
    auto emptyArray3Val = sbe::value::makeNewArray().second;

    // Create a small CEHistogram with boolean & empty array counts using maxdiff.
    const std::vector<SBEValue> values{
        // Scalar doubles: 1, 2, 3.
        SBEValue{sbe::value::TypeTags::NumberDouble, sbe::value::bitcastFrom<double>(1.0)},
        SBEValue{sbe::value::TypeTags::NumberDouble, sbe::value::bitcastFrom<double>(2.0)},
        SBEValue{sbe::value::TypeTags::NumberDouble, sbe::value::bitcastFrom<double>(3.0)},
        // 5x booleans: 2 true, 4 false.
        SBEValue{sbe::value::TypeTags::Boolean, true},
        SBEValue{sbe::value::TypeTags::Boolean, true},
        SBEValue{sbe::value::TypeTags::Boolean, false},
        SBEValue{sbe::value::TypeTags::Boolean, false},
        SBEValue{sbe::value::TypeTags::Boolean, false},
        SBEValue{sbe::value::TypeTags::Boolean, false},
        // 3x empty arrays.
        SBEValue{sbe::value::TypeTags::Array, emptyArray1Val},
        SBEValue{sbe::value::TypeTags::Array, emptyArray2Val},
        SBEValue{sbe::value::TypeTags::Array, emptyArray3Val},
        // A non-empty array.
        SBEValue{sbe::value::TypeTags::Array, nonEmptyArrayVal},
    };
    auto ceHist = createCEHistogram(values, numDocs);
    auto expectedSerialized = ceHist->serialize();

    // Sanity check counters.
    ASSERT_EQ(ceHist->getTrueCount(), 2.0);
    ASSERT_EQ(ceHist->getFalseCount(), 4.0);
    ASSERT_EQ(ceHist->getEmptyArrayCount(), 3.0);

    // Serialize histogram into a stats path.
    std::string path = "somePath";
    constexpr double sampleRate = 1.0;
    auto serialized = stats::makeStatsPath(path, numDocs, sampleRate, ceHist);

    // Initalize stats collection.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "stats");
    std::string statsColl(StatsCacheLoader::kStatsPrefix + "." + nss.coll());
    NamespaceString statsNss =
        NamespaceString::createNamespaceString_forTest(nss.db_forTest(), statsColl);
    createStatsCollection(statsNss);

    // Write serialized stats path to collection.
    AutoGetCollection autoColl(operationContext(), statsNss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(serialized), nullptr));
        wuow.commit();
    }

    // Read stats path & verify values are consistent with what we expect.
    auto actualAH = _statsCacheLoader.getStats(operationContext(), std::make_pair(nss, path)).get();
    auto actualSerialized = actualAH->serialize();

    // Sanity check counters.
    ASSERT_EQ(actualAH->getTrueCount(), 2.0);
    ASSERT_EQ(actualAH->getFalseCount(), 4.0);
    ASSERT_EQ(actualAH->getEmptyArrayCount(), 3.0);

    ASSERT_BSONOBJ_EQ(expectedSerialized, actualSerialized);
}

}  // namespace
}  // namespace mongo::stats
