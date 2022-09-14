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

#include "mongo/bson/oid.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/ce/stats_cache_loader_impl.h"
#include "mongo/db/query/ce/stats_cache_loader_test_fixture.h"
#include "mongo/db/query/ce/stats_gen.h"
#include "mongo/db/query/ce/stats_serialization_utils.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {
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

TEST_F(StatsCacheLoaderTest, VerifyStatsLoad) {

    NamespaceString nss("test", "stats");

    std::string statsColl(StatsCacheLoader::kStatsPrefix + "." + nss.coll());
    NamespaceString statsNss(nss.db(), statsColl);

    std::list<BSONObj> buckets;
    for (long long i = 1; i <= 3; i++) {
        auto typeValue = stats_serialization_utils::TypeValuePair(
            sbe::value::TypeTags::NumberDouble, double{i + 1.0});

        auto bucket = stats_serialization_utils::makeStatsBucket(typeValue, i, i, i, 3 * i, i + 2);
        buckets.push_back(bucket);
    }
    stats_serialization_utils::TypeCount types;
    for (long long i = 1; i <= 3; i++) {
        std::stringstream typeName;
        typeName << "type" << i;
        auto typeElem = std::pair<std::string, long>(typeName.str(), i);
        types.push_back(typeElem);
    }
    auto serializedPath = stats_serialization_utils::makeStatsPath(
        "somePath", 100, 10, 0.1, 10, std::make_pair(4LL, 6LL), types, buckets, boost::none);

    createStatsCollection(statsNss);

    AutoGetCollection autoColl(operationContext(), statsNss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    {
        WriteUnitOfWork wuow(operationContext());

        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(serializedPath), nullptr));
        wuow.commit();
    }
    auto newStats =
        _statsCacheLoader.getStats(operationContext(), std::make_pair(nss, "somePath")).get();
    std::cout << newStats->toString() << std::endl;
}

}  // namespace
}  // namespace mongo
