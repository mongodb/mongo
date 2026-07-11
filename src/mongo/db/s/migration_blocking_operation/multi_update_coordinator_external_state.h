// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/dbmessage.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/util/modules.h"
namespace mongo {

class MultiUpdateCoordinatorExternalState {
public:
    virtual Future<DbResponse> sendClusterUpdateCommandToShards(OperationContext* opCtx,
                                                                const Message& message) const = 0;
    virtual void startBlockingMigrations(OperationContext* opCtx,
                                         const MultiUpdateCoordinatorMetadata& metadata) = 0;
    virtual void stopBlockingMigrations(OperationContext* opCtx,
                                        const MultiUpdateCoordinatorMetadata& metadata) = 0;
    virtual bool isUpdatePending(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 AggregateCommandRequest& request) const = 0;
    virtual bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) const = 0;
    virtual void createCollection(OperationContext* opCtx, const NamespaceString& nss) const = 0;

    virtual InternalSessionPool::Session acquireSession() = 0;
    virtual void releaseSession(InternalSessionPool::Session session) = 0;

    virtual ~MultiUpdateCoordinatorExternalState() = default;
};

class MultiUpdateCoordinatorExternalStateImpl : public MultiUpdateCoordinatorExternalState {
public:
    MultiUpdateCoordinatorExternalStateImpl(InternalSessionPool* sessionPool);

    Future<DbResponse> sendClusterUpdateCommandToShards(OperationContext* opCtx,
                                                        const Message& message) const override;
    void startBlockingMigrations(OperationContext* opCtx,
                                 const MultiUpdateCoordinatorMetadata& metadata) override;
    void stopBlockingMigrations(OperationContext* opCtx,
                                const MultiUpdateCoordinatorMetadata& metadata) override;
    bool isUpdatePending(OperationContext* opCtx,
                         const NamespaceString& nss,
                         AggregateCommandRequest& request) const override;
    bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) const override;
    void createCollection(OperationContext* opCtx, const NamespaceString& nss) const override;

    InternalSessionPool::Session acquireSession() override;
    void releaseSession(InternalSessionPool::Session session) override;

private:
    InternalSessionPool* _sessionPool;
};

class MultiUpdateCoordinatorExternalStateFactory {
public:
    virtual std::unique_ptr<MultiUpdateCoordinatorExternalState> createExternalState() const = 0;
    virtual ~MultiUpdateCoordinatorExternalStateFactory() {}
};

class MultiUpdateCoordinatorExternalStateFactoryImpl
    : public MultiUpdateCoordinatorExternalStateFactory {
public:
    MultiUpdateCoordinatorExternalStateFactoryImpl(ServiceContext* serviceContext);

    std::unique_ptr<MultiUpdateCoordinatorExternalState> createExternalState() const override;

private:
    ServiceContext* _serviceContext;
};

}  // namespace mongo
