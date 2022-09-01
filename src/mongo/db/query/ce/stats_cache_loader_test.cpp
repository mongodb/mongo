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

    std::string statsColl(StatsCacheLoader::kStatsPrefix + "." + nss.ns());
    NamespaceString statsNss(StatsCacheLoader::kStatsDb, statsColl);

    createStatsCollection(statsNss);

    AutoGetCollection autoColl(operationContext(), statsNss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    {
        WriteUnitOfWork wuow(operationContext());
        // TODO: SERVER-69238, insert histogram.
        BSONObj doc = BSON("_id" << 1);
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(doc), nullptr));
        wuow.commit();
    }
    auto newStats = _statsCacheLoader.getStats(operationContext(), nss).get();
    // TODO: SERVER-69238, verify histogram.
}

}  // namespace
}  // namespace mongo
