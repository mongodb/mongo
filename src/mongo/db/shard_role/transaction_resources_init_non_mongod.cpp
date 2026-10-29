// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"

#include <memory>
#include <string>

namespace mongo {
namespace {

class TransactionResourcesNonMongoDClientObserver : public ServiceContext::ClientObserver {
public:
    TransactionResourcesNonMongoDClientObserver() = default;
    ~TransactionResourcesNonMongoDClientObserver() override = default;

    void onCreateClient(Client* client) final {}

    void onDestroyClient(Client* client) final {}

    void onCreateOperationContext(OperationContext* opCtx) final {
        shard_role_details::makeLockerOnOperationContext(opCtx);
    }

    void onDestroyOperationContext(OperationContext* opCtx) final {}
};

}  // namespace

ServiceContext::ConstructorActionRegisterer transactionResourcesConstructor{
    "TransactionResourcesConstructor", [](ServiceContext* service) {
        service->registerClientObserver(
            std::make_unique<TransactionResourcesNonMongoDClientObserver>());
    }};

}  // namespace mongo
