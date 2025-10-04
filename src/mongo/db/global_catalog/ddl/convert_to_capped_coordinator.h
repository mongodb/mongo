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

#include "mongo/db/global_catalog/ddl/convert_to_capped_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"

namespace mongo {

class ConvertToCappedCoordinator final
    : public RecoverableShardingDDLCoordinator<ConvertToCappedCoordinatorDocument,
                                               ConvertToCappedCoordinatorPhaseEnum> {
public:
    using StateDoc = ConvertToCappedCoordinatorDocument;
    using Phase = ConvertToCappedCoordinatorPhaseEnum;

    ConvertToCappedCoordinator(ShardingDDLCoordinatorService* service,
                               const BSONObj& initialStateDoc)
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

private:
    StringData serializePhase(const Phase& phase) const override {
        return ConvertToCappedCoordinatorPhase_serializer(phase);
    }

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
        const CancellationToken& token,
        CriticalSectionBlockTypeEnum blockType);

    void _exitCriticalSectionOnDataShard(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    void _performNoopRetryableWriteOnParticipantShardsAndConfigsvr(
        OperationContext* opCtx,
        const OperationSessionInfo& osi,
        const std::shared_ptr<executor::TaskExecutor>& executor);

    const mongo::ShardsvrConvertToCappedRequest _request;

    const BSONObj _critSecReason;
};

}  // namespace mongo
