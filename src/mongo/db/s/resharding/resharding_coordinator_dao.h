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
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/write_ops/batched_command_request.h"

#include <memory>

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

class DaoStorageClientFactory {
public:
    virtual ~DaoStorageClientFactory() = default;
    virtual std::unique_ptr<DaoStorageClient> createDaoStorageClient(
        boost::optional<TxnNumber> txnNumber) = 0;
};

class DaoStorageClientFactoryImpl : public DaoStorageClientFactory {
public:
    std::unique_ptr<DaoStorageClient> createDaoStorageClient(
        boost::optional<TxnNumber> txnNumber) override {
        if (txnNumber) {
            return std::make_unique<TransactionalDaoStorageClientImpl>(txnNumber.get());
        }
        return std::make_unique<DaoStorageClientImpl>();
    }
};

class ReshardingCoordinatorDao {
public:
    explicit ReshardingCoordinatorDao(const UUID& reshardingUUID,
                                      std::unique_ptr<DaoStorageClientFactory> clientFactory =
                                          std::make_unique<DaoStorageClientFactoryImpl>())
        : _reshardingUUID(reshardingUUID), _clientFactory(std::move(clientFactory)) {}
    ~ReshardingCoordinatorDao() = default;

    CoordinatorStateEnum getPhase(OperationContext* opCtx,
                                  boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToPreparingToDonatePhase(
        OperationContext* opCtx,
        ParticipantShardsAndChunks shardsAndChunks,
        boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToCloningPhase(
        OperationContext* opCtx,
        Date_t now,
        Timestamp cloneTimestamp,
        ReshardingApproxCopySize approxCopySize,
        boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToApplyingPhase(
        OperationContext* opCtx, Date_t now, boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToBlockingWritesPhase(
        OperationContext* opCtx,
        Date_t now,
        Date_t criticalSectionExpireTime,
        boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToAbortingPhase(
        OperationContext* opCtx,
        Date_t now,
        Status abortReason,
        boost::optional<TxnNumber> txnNumber = boost::none);

private:
    const UUID _reshardingUUID;
    std::unique_ptr<DaoStorageClientFactory> _clientFactory;
};

}  // namespace resharding
}  // namespace mongo
