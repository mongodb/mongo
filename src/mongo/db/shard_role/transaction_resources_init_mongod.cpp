// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
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
