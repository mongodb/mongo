/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/catalog/database_holder_mock.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/logv2/log_domain_global.h"

namespace mongo {
namespace {

MONGO_INITIALIZER_GENERAL(CoreOptions_Store, (), ())
(InitializerContext* context) {
    // Dummy initializer to fill in the initializer graph
}

MONGO_INITIALIZER_GENERAL(DisableLogging, (), ())
(InitializerContext*) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

class StorageInterfaceMockTimestamp : public repl::StorageInterfaceMock {
public:
    boost::optional<BSONObj> findOplogEntryLessThanOrEqualToTimestampRetryOnWCE(
        OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) override {
        auto now = Date_t::now();
        return BSON("ts" << Timestamp(now) << "t" << int64_t{5} << "wall" << now);
    }

    Status upsertById(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      const BSONElement& idKey,
                      const BSONObj& update) override {
        return Status::OK();
    }
};

ServiceContext* setupServiceContext() {
    auto serviceContext = ServiceContext::make();
    auto serviceContextPtr = serviceContext.get();
    setGlobalServiceContext(std::move(serviceContext));
    return serviceContextPtr;
}

void BM_refreshOplogTruncateAFterPointIfPrimary(benchmark::State& state) {
    auto* serviceContext = setupServiceContext();
    StorageInterfaceMockTimestamp storageInterface;
    repl::ReplicationCoordinator::set(
        serviceContext,
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext, &storageInterface));
    repl::ReplicationConsistencyMarkersImpl consistencyMarkers(&storageInterface);
    consistencyMarkers.startUsingOplogTruncateAfterPointForPrimary();
    auto client =
        serviceContext->getService()->makeClient("BM_refresOplogTruncateAfterPoint_Client");
    auto opCtx = client->makeOperationContext();
    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderMock>());
    for (auto _ : state) {
        consistencyMarkers.refreshOplogTruncateAfterPointIfPrimary(opCtx.get());
    }
    consistencyMarkers.stopUsingOplogTruncateAfterPointForPrimary();
}

BENCHMARK(BM_refreshOplogTruncateAFterPointIfPrimary)->MinTime(10.0);

}  // namespace
}  // namespace mongo
