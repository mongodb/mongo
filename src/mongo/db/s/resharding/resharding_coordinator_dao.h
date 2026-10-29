// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>

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

    /*
     * Updates every donor shards' DocumentsToCopy value on coordinator document in the cloning
     * phase. Will invariant if called in any other phase.
     */
    ReshardingCoordinatorDocument updateNumberOfDocsToCopy(
        OperationContext* opCtx,
        const std::map<ShardId, int64_t>& documentsToCopy,
        boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToApplyingPhase(
        OperationContext* opCtx, Date_t now, boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToBlockingWritesPhase(
        OperationContext* opCtx,
        Date_t now,
        Date_t criticalSectionExpireTime,
        boost::optional<TxnNumber> txnNumber = boost::none);

    /*
     * Updates every donor shards' DocumentsFinal value on coordinator document in the blocking
     * writes phase. Will invariant if called in any other phase.
     */
    ReshardingCoordinatorDocument updateNumberOfDocsCopiedFinal(
        OperationContext* opCtx,
        const std::map<ShardId, int64_t>& documentsCopiedFinal,
        boost::optional<TxnNumber> txnNumber = boost::none);

    /*
     * Updates every recipient shard's documentsFinal value on the coordinator document in the
     * blocking writes phase. Will invariant if called in any other phase.
     */
    ReshardingCoordinatorDocument updateRecipientDocumentsFinal(
        OperationContext* opCtx,
        const std::map<ShardId, int64_t>& recipientDocumentsFinal,
        boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument transitionToAbortingPhase(
        OperationContext* opCtx,
        Date_t now,
        Status abortReason,
        boost::optional<TxnNumber> txnNumber = boost::none);

    ReshardingCoordinatorDocument updateSession(
        OperationContext* opCtx, const boost::optional<CoordinatorSession>& newSession);

private:
    const UUID _reshardingUUID;
    std::unique_ptr<DaoStorageClientFactory> _clientFactory;
    BSONObjBuilder _documentsCopyUpdateBuilder(const ReshardingCoordinatorDocument& doc,
                                               const std::map<ShardId, int64_t>& documents,
                                               std::string_view numberOfDocsFieldName);
};

}  // namespace resharding
}  // namespace mongo
