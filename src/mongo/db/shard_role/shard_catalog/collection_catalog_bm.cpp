// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

ServiceContext* setupServiceContext() {
    auto serviceContext = ServiceContext::make();
    auto serviceContextPtr = serviceContext.get();
    setGlobalServiceContext(std::move(serviceContext));
    return serviceContextPtr;
}

void createCollections(OperationContext* opCtx, int numCollections) {
    Lock::GlobalLock globalLk(opCtx, MODE_X);

    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        for (auto i = 0; i < numCollections; i++) {
            const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                "collection_catalog_bm", std::to_string(i));
            catalog.registerCollection(opCtx,
                                       std::make_shared<CollectionMock>(nss),
                                       /*ts=*/boost::none);
        }
    });
}

}  // namespace

void BM_CollectionCatalogWrite(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext->getService());
    ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();

    createCollections(opCtx.get(), state.range(0));

    Lock::GlobalLock lk{opCtx.get(), MODE_IX};

    for (auto _ : state) {
        benchmark::ClobberMemory();
        CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& catalog) {});
    }
}

void BM_CollectionCatalogCreateDropCollection(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext->getService());
    ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();
    Lock::GlobalLock globalLk(opCtx.get(), MODE_X);

    createCollections(opCtx.get(), state.range(0));

    for (auto _ : state) {
        benchmark::ClobberMemory();
        CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& catalog) {
            const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                "collection_catalog_bm", std::to_string(state.range(0)));
            const UUID uuid = UUID::gen();
            catalog.registerCollection(
                opCtx.get(), std::make_shared<CollectionMock>(uuid, nss), boost::none);
            catalog.deregisterCollection(opCtx.get(), uuid, boost::none);
        });
    }
}

void BM_CollectionCatalogCreateNCollections(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();

        auto serviceContext = setupServiceContext();
        ThreadClient threadClient(serviceContext->getService());
        ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);

        auto numCollections = state.range(0);
        for (auto i = 0; i < numCollections; i++) {
            const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                "collection_catalog_bm", std::to_string(i));
            CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    opCtx.get(), std::make_shared<CollectionMock>(nss), boost::none);
            });
        }
    }
}

void BM_CollectionCatalogLookupCollectionByNamespace(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext->getService());
    ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();

    createCollections(opCtx.get(), state.range(0));
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "collection_catalog_bm", std::to_string(state.range(0) / 2));

    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto coll =
            CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
        invariant(coll);
    }
}

void BM_CollectionCatalogLookupCollectionByUUID(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext->getService());
    ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();

    createCollections(opCtx.get(), state.range(0));
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "collection_catalog_bm", std::to_string(state.range(0) / 2));
    auto coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
    invariant(coll->ns() == nss);
    const UUID uuid = coll->uuid();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto res = CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), uuid);
        invariant(res == coll);
    }
}

void BM_CollectionCatalogIterateCollections(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext->getService());
    ServiceContext::UniqueOperationContext opCtx = threadClient->makeOperationContext();

    createCollections(opCtx.get(), state.range(0));

    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto catalog = CollectionCatalog::get(opCtx.get());
        auto count = 0;
        for ([[maybe_unused]] auto&& coll : catalog->range(
                 DatabaseName::createDatabaseName_forTest(boost::none, "collection_catalog_bm"))) {
            benchmark::DoNotOptimize(count++);
        }
    }
}

BENCHMARK(BM_CollectionCatalogWrite)->Arg(1)->Arg(1000)->Arg(100'000);
BENCHMARK(BM_CollectionCatalogCreateDropCollection)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_CollectionCatalogCreateNCollections)->Ranges({{{1}, {32'768}}});
BENCHMARK(BM_CollectionCatalogLookupCollectionByNamespace)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_CollectionCatalogLookupCollectionByUUID)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_CollectionCatalogIterateCollections)->Ranges({{{1}, {100'000}}});

}  // namespace mongo
