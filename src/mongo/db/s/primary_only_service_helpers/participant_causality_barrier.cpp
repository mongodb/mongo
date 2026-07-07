/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/primary_only_service_helpers/participant_causality_barrier.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"

namespace mongo {

ParticipantCausalityBarrier::ParticipantCausalityBarrier(
    std::vector<ShardId> participants,
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken token)
    : _participants{std::move(participants)},
      _executor{std::move(executor)},
      _token{std::move(token)} {}

void ParticipantCausalityBarrier::perform(OperationContext* opCtx,
                                          const OperationSessionInfo& osi) {
    _performNoopRetryableWriteOnShards(opCtx, _participants, osi, _executor, _token);
}

void ParticipantCausalityBarrier::_performNoopRetryableWriteOnShards(
    OperationContext* opCtx,
    const std::vector<ShardId>& shardIds,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const CancellationToken& token) {
    auto updateOp = sharding_ddl_util::buildNoopWriteRequestCommand();
    generic_argument_util::setOperationSessionInfo(updateOp, osi);
    generic_argument_util::setMajorityWriteConcern(updateOp);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<write_ops::UpdateCommandRequest>>(
        executor, token, updateOp);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}


}  // namespace mongo
