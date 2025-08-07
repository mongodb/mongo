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

#include "mongo/db/query/query_bm_fixture.h"

#include "mongo/transport/service_entry_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

const NamespaceString QueryBenchmarkFixture::kNss =
    NamespaceString::createNamespaceString_forTest("test", "coll");

void QueryBenchmarkFixture::setUpSharedResources(benchmark::State& state) {
    _fixture.emplace(CatalogScopedGlobalServiceContextForTest::Options{}, false);

    ReadWriteConcernDefaults::create(getGlobalServiceContext()->getService(),
                                     _lookupMock.getFetchDefaultsFn());
    _lookupMock.setLookupCallReturnValue({});

    populateCollection(state.range(0), state.range(1));
}

void QueryBenchmarkFixture::tearDownSharedResources(benchmark::State& state) {
    _fixture.reset();
    _docs.clear();
}

void QueryBenchmarkFixture::runBenchmark(BSONObj filter,
                                         BSONObj projection,
                                         benchmark::State& state) {
    BSONObj command = BSON("find" << kNss.coll() << "$db" << kNss.db_forTest() << "filter" << filter
                                  << "projection" << projection);
    OpMsgRequest request;
    request.body = command;
    auto msg = request.serialize();

    ThreadClient threadClient{getGlobalServiceContext()->getService()};
    runBenchmarkWithProfiler(
        [&]() {
            Client& client = cc();
            auto opCtx = client.makeOperationContext();
            auto statusWithResponse =
                client.getService()
                    ->getServiceEntryPoint()
                    ->handleRequest(opCtx.get(), msg, opCtx->fastClockSource().now())
                    .getNoThrow();
            iassert(statusWithResponse);
            LOGV2_DEBUG(9278700,
                        1,
                        "db response",
                        "request"_attr = msg.opMsgDebugString(),
                        "response"_attr =
                            statusWithResponse.getValue().response.opMsgDebugString());
        },
        state);
}

void QueryBenchmarkFixture::createIndexes(OperationContext* opCtx, std::vector<BSONObj> indexes) {
    for (const auto& indexSpec : indexes) {
        auto acquisition = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter coll(opCtx, &acquisition);

        WriteUnitOfWork wunit(opCtx);
        uassertStatusOK(
            coll.getWritableCollection(opCtx)
                ->getIndexCatalog()
                ->createIndexOnEmptyCollection(opCtx, coll.getWritableCollection(opCtx), indexSpec)
                .getStatus());
        wunit.commit();
    }
}

void QueryBenchmarkFixture::populateCollection(size_t size, size_t approximateSize) {
    std::vector<InsertStatement> inserts;
    for (size_t i = 0; i < size; ++i) {
        _docs.push_back(generateDocument(i, approximateSize));
        inserts.emplace_back(_docs.back());
    }

    ThreadClient threadClient{getGlobalServiceContext()->getService()};
    auto opCtx = cc().makeOperationContext();
    auto storage = repl::StorageInterface::get(opCtx.get());

    uassertStatusOK(storage->createCollection(opCtx.get(), kNss, CollectionOptions{}));
    createIndexes(opCtx.get(), getIndexSpecs());
    uassertStatusOK(storage->insertDocuments(opCtx.get(), kNss, inserts));
}

}  // namespace mongo
