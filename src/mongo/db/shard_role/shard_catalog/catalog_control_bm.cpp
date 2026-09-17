// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/catalog_control.h"

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/storage/mdb_catalog.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {
void BM_CatalogControlReopen(benchmark::State& state) {
    logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
        mongo::logv2::LogComponent::kDefault, mongo::logv2::LogSeverity::Error());

    MongoDScopedGlobalServiceContextForTest tsc(
        MongoDScopedGlobalServiceContextForTest::Options().engine("wiredTiger"), false);
    ThreadClient threadClient(tsc.getService());

    ServiceContext::UniqueOperationContext opCtx = Client::getCurrent()->makeOperationContext();
    auto svcCtx = opCtx->getServiceContext();

    {
        auto mdbCatalog = svcCtx->getStorageEngine()->getMDBCatalog();
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);

        WriteUnitOfWork wuow(opCtx.get());
        for (auto i = 0; i < state.range(0); i++) {
            const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                "catalog_control_bm", std::to_string(i));
            CollectionOptions options;
            options.uuid = UUID::gen();
            const auto ident = ident::generateNewCollectionIdent(nss.dbName(), false, false);
            const auto catalogId = mdbCatalog->reserveCatalogId(opCtx.get());
            uassertStatusOK(durable_catalog::createCollection(
                opCtx.get(), catalogId, nss, ident, options, mdbCatalog));
        }
        wuow.commit();
    }

    auto stableTimestamp = repl::StorageInterfaceImpl().getLastStableRecoveryTimestamp(svcCtx);

    // Since we populated the durable catalog directly the first reopen will be faster due to not
    // having to tear down as many in-memory things
    {
        benchmark::ClobberMemory();
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        auto catalogState = catalog::closeCatalog(opCtx.get());
        catalog::openCatalogAfterRollbackToStable(opCtx.get(), catalogState, *stableTimestamp);
    }

    for (auto _ : state) {
        benchmark::ClobberMemory();
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        auto catalogState = catalog::closeCatalog(opCtx.get());
        catalog::openCatalogAfterRollbackToStable(opCtx.get(), catalogState, *stableTimestamp);
    }
}

}  // namespace

BENCHMARK(BM_CatalogControlReopen)->Ranges({{{1}, {4096}}});

}  // namespace mongo
