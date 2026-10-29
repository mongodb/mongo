// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_bm_fixture.h"

#include "mongo/db/shard_role/shard_role.h"
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
