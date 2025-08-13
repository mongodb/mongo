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

#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"

#include <memory>
#include <string>

namespace mongo {
namespace {

class TransactionResourcesMongoDClientObserver : public ServiceContext::ClientObserver {
public:
    TransactionResourcesMongoDClientObserver() = default;
    ~TransactionResourcesMongoDClientObserver() override = default;

    void onCreateClient(Client* client) final {}

    void onDestroyClient(Client* client) final {}

    void onCreateOperationContext(OperationContext* opCtx) final {
        auto service = opCtx->getServiceContext();

        shard_role_details::TransactionResources::attachToOpCtx(
            opCtx, std::make_unique<shard_role_details::TransactionResources>());
        shard_role_details::makeLockerOnOperationContext(opCtx);

        // There are a few cases where we don't have a storage engine available yet when creating an
        // operation context.
        // 1. During startup, we create an operation context to allow the storage engine
        //    initialization code to make use of the lock manager.
        // 2. There are unit tests that create an operation context before initializing the storage
        //    engine.
        // 3. Unit tests that use an operation context but don't require a storage engine for their
        //    testing purpose.
        auto storageEngine = service->getStorageEngine();
        if (storageEngine) {
            shard_role_details::setRecoveryUnit(
                opCtx,
                storageEngine->newRecoveryUnit(),
                WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        }
    }

    void onDestroyOperationContext(OperationContext* opCtx) final {}
};

}  // namespace

ServiceContext::ConstructorActionRegisterer transactionResourcesConstructor{
    "TransactionResourcesConstructor", [](ServiceContext* service) {
        service->registerClientObserver(
            std::make_unique<TransactionResourcesMongoDClientObserver>());
    }};

}  // namespace mongo
