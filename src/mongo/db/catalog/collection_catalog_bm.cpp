/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {

class LockerImplClientObserver : public ServiceContext::ClientObserver {
public:
    LockerImplClientObserver() = default;
    ~LockerImplClientObserver() = default;

    void onCreateClient(Client* client) final {}

    void onDestroyClient(Client* client) final {}

    void onCreateOperationContext(OperationContext* opCtx) override {
        opCtx->setLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    }

    void onDestroyOperationContext(OperationContext* opCtx) final {}
};

const ServiceContext::ConstructorActionRegisterer clientObserverRegisterer{
    "CollectionCatalogBenchmarkClientObserver",
    [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<LockerImplClientObserver>());
    },
    [](ServiceContext* serviceContext) {
    }};

ServiceContext* setupServiceContext() {
    auto serviceContext = ServiceContext::make();
    auto serviceContextPtr = serviceContext.get();
    setGlobalServiceContext(std::move(serviceContext));
    return serviceContextPtr;
}

void createCollections(OperationContext* opCtx, int numCollections) {
    Lock::GlobalLock globalLk(opCtx, MODE_X);
    BatchedCollectionCatalogWriter batched(opCtx);

    for (auto i = 0; i < numCollections; i++) {
        const NamespaceString nss("collection_catalog_bm", std::to_string(i));
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerCollection(opCtx,
                                       UUID::gen(),
                                       std::make_shared<CollectionMock>(nss),
                                       /*ts=*/boost::none);
        });
    }
}

}  // namespace

void BM_CollectionCatalogWrite(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext);
    ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();

    createCollections(opCtx.get(), state.range(0));

    for (auto _ : state) {
        benchmark::ClobberMemory();
        CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& catalog) {});
    }
}

void BM_CollectionCatalogWriteBatchedWithGlobalExclusiveLock(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext);
    ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();

    // TODO(SERVER-74657): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(*threadClient.get());
        threadClient.get()->setSystemOperationUnkillableByStepdown(lk);
    }

    createCollections(opCtx.get(), state.range(0));

    Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
    BatchedCollectionCatalogWriter batched(opCtx.get());

    for (auto _ : state) {
        benchmark::ClobberMemory();
        CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& catalog) {});
    }
}

BENCHMARK(BM_CollectionCatalogWrite)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_CollectionCatalogWriteBatchedWithGlobalExclusiveLock)->Ranges({{{1}, {100'000}}});

}  // namespace mongo
