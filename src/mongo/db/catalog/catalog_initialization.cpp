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

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/repl/replica_set_aware_service.h"

namespace mongo {

class CatalogInitializationService;

namespace {
const auto catalogInitializationServiceDecoration =
    ServiceContext::declareDecoration<CatalogInitializationService>();

const ReplicaSetAwareServiceRegistry::Registerer<CatalogInitializationService>
    catalogInitializationServiceRegisterer("CatalogInitializationService");

}  // namespace

class CatalogInitializationService : public ReplicaSetAwareService<CatalogInitializationService> {
public:
    CatalogInitializationService() = default;
    ~CatalogInitializationService() override = default;

    static CatalogInitializationService* get(ServiceContext* serviceContext) {
        return &(catalogInitializationServiceDecoration(serviceContext));
    }

private:
    void onInitialDataAvailable(OperationContext* opCtx, bool isMajorityDataAvailable) override {
        for (const auto& dbName : CollectionCatalog::get(opCtx)->getAllDbNames()) {
            Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
            Lock::CollectionLock sysCollLock(
                opCtx, NamespaceString::makeSystemDotViewsNamespace(dbName), MODE_X);
            WriteUnitOfWork wuow(opCtx);
            CollectionCatalog::get(opCtx)->reloadViews(opCtx, dbName);
            wuow.commit();
        }
    }

    std::string getServiceName() const final {
        return "CatalogInitializationService";
    }

    void onStartup(OperationContext* opCtx) override {}
    void onSetCurrentConfig(OperationContext* opCtx) override {}
    void onShutdown() override {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override {}
    void onStepUpComplete(OperationContext* opCtx, long long term) override {}
    void onStepDown() override {}
    void onRollback() override {}
    void onBecomeArbiter() override {}
};

}  // namespace mongo
