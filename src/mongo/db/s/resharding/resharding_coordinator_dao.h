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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"

namespace mongo {
namespace resharding {

class DaoStorageClient {
public:
    virtual ~DaoStorageClient() = default;

    virtual void alterState(OperationContext* opCtx, const BatchedCommandRequest& request) = 0;
    virtual ReshardingCoordinatorDocument readState(OperationContext* opCtx,
                                                    const UUID& reshardingUUID) = 0;
};

class TransactionalDaoStorageClientImpl : public DaoStorageClient {
public:
    TransactionalDaoStorageClientImpl(TxnNumber txnNumber) : _txnNumber{txnNumber} {}

    void alterState(OperationContext* opCtx, const BatchedCommandRequest& request) override;
    ReshardingCoordinatorDocument readState(OperationContext* opCtx,
                                            const UUID& reshardingUUID) override;

private:
    TxnNumber _txnNumber;
};

class DaoStorageClientImpl : public DaoStorageClient {
public:
    void alterState(OperationContext* opCtx, const BatchedCommandRequest& request) override;
    ReshardingCoordinatorDocument readState(OperationContext* opCtx,
                                            const UUID& reshardingUUID) override;
};

class ReshardingCoordinatorDao {
public:
    ReshardingCoordinatorDao() = default;
    ~ReshardingCoordinatorDao() = default;

    CoordinatorStateEnum getPhase(OperationContext* opCtx,
                                  DaoStorageClient* client,
                                  const UUID& reshardingUUID) {
        return client->readState(opCtx, reshardingUUID).getState();
    }

    ReshardingCoordinatorDocument transitionToCloningPhase(OperationContext* opCtx,
                                                           DaoStorageClient* client,
                                                           Date_t now,
                                                           Timestamp cloneTimestamp,
                                                           ReshardingApproxCopySize approxCopySize,
                                                           const UUID& reshardingUUID);

    ReshardingCoordinatorDocument transitionToApplyingPhase(OperationContext* opCtx,
                                                            DaoStorageClient* client,
                                                            Date_t now,
                                                            const UUID& reshardingUUID);

    ReshardingCoordinatorDocument transitionToBlockingWritesPhase(OperationContext* opCtx,
                                                                  DaoStorageClient* client,
                                                                  Date_t now,
                                                                  Date_t criticalSectionExpireTime,
                                                                  const UUID& reshardingUUID);
};

}  // namespace resharding
}  // namespace mongo
