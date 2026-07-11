// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/convert_to_capped_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/participant_block_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

class ConvertToCappedCoordinator final
    : public RecoverableShardingDDLCoordinator<ConvertToCappedCoordinatorDocument> {
public:
    ConvertToCappedCoordinator(ShardingCoordinatorService* service, const BSONObj& initialStateDoc)
        : RecoverableShardingDDLCoordinator(service, "ConvertToCappedCoordinator", initialStateDoc),
          _request(_doc.getShardsvrConvertToCappedRequest()),
          _critSecReason(BSON("convertToCapped" << NamespaceStringUtil::serialize(
                                  nss(), SerializationContext::stateDefault()))) {};

    void checkIfOptionsConflict(const BSONObj& doc) const override {
        const auto otherDoc = ConvertToCappedCoordinatorDocument::parse(
            doc, IDLParserContext("ConvertToCappedCoordinatorDocument"));

        const auto& selfReq = _request.toBSON();
        const auto& otherReq = otherDoc.getShardsvrConvertToCappedRequest().toBSON();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Another convertToCapped is already running for the same "
                                 "namespace with size set to "
                              << _request.getSize(),
                SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
    };

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override {
        cmdInfoBuilder->append(
            "convertToCapped",
            NamespaceStringUtil::serialize(nss(), SerializationContext::stateDefault()));
        cmdInfoBuilder->appendElements(_request.toBSON());
    };

protected:
    logv2::DynamicAttributes getCoordinatorLogAttrs() const override;

    bool isInCriticalSection(Phase phase) const override;

private:
    bool _mustAlwaysMakeProgress() override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _checkPreconditions(OperationContext* opCtx);

    void _enterCriticalSectionOnDataShard(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    void _exitCriticalSectionOnDataShard(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    std::vector<ShardId> _getParticipantShards(OperationContext* opCtx);

    const mongo::ShardsvrConvertToCappedRequest _request;

    const BSONObj _critSecReason;
};

}  // namespace mongo
