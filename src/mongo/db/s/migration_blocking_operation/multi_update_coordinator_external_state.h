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

#pragma once

#include "mongo/db/dbmessage.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/internal_session_pool.h"

namespace mongo {

class MultiUpdateCoordinatorExternalState {
public:
    virtual Future<DbResponse> sendClusterUpdateCommandToShards(OperationContext* opCtx,
                                                                const Message& message) const = 0;
    virtual void startBlockingMigrations(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const UUID& operationId) = 0;
    virtual void stopBlockingMigrations(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& operationId) = 0;
    virtual bool isUpdatePending(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 AggregateCommandRequest& request) const = 0;
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
                                 const NamespaceString& nss,
                                 const UUID& operationId) override;
    void stopBlockingMigrations(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const UUID& operationId) override;
    bool isUpdatePending(OperationContext* opCtx,
                         const NamespaceString& nss,
                         AggregateCommandRequest& request) const override;
    virtual InternalSessionPool::Session acquireSession() override;
    virtual void releaseSession(InternalSessionPool::Session session) override;

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
