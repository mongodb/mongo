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

#include "mongo/db/local_catalog/catalog_control.h"

#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
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
        catalog::openCatalog(opCtx.get(), catalogState, *stableTimestamp);
    }

    for (auto _ : state) {
        benchmark::ClobberMemory();
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        auto catalogState = catalog::closeCatalog(opCtx.get());
        catalog::openCatalog(opCtx.get(), catalogState, *stableTimestamp);
    }
}

}  // namespace

BENCHMARK(BM_CatalogControlReopen)->Ranges({{{1}, {4096}}});

}  // namespace mongo
