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

#include "mongo/db/s/add_shard_coordinator.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ExecutorFuture<void> AddShardCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kCheckLocalPreconditions,
            [this, _ = shared_from_this()]() {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                _verifyInput();

                const auto existingShard =
                    uassertStatusOK(topology_change_helpers::checkIfShardExists(
                        opCtx,
                        _doc.getConnectionString(),
                        _doc.getProposedName(),
                        *ShardingCatalogManager::get(opCtx)->localCatalogClient()));
                if (existingShard.has_value()) {
                    _doc.setChosenName(existingShard.value().getName());
                    _enterPhase(AddShardCoordinatorPhaseEnum::kFinal);
                }
            }))
        .then(_buildPhaseHandler(Phase::kFinal,
                                 [this, _ = shared_from_this()]() {
                                     auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();

                                     // TODO this should not happen later on. if we reach the final
                                     // phase that means we added something (or was already added).
                                     // If we were not able to add anything then an assert should
                                     // had been thrown earlier.
                                     // invariant(_doc.getChosenName().has_value());
                                     uassert(
                                         ErrorCodes::NotImplemented,
                                         "something is still missing here in the implementation...",
                                         _doc.getChosenName().has_value());

                                     repl::ReplClientInfo::forClient(opCtx->getClient())
                                         .setLastOpToSystemLastOpTime(opCtx);

                                     _result = _doc.getChosenName().value().toString();
                                 }))
        .onError([this, _ = shared_from_this()](const Status& status) { return status; });
}

const std::string& AddShardCoordinator::getResult(OperationContext* opCtx) const {
    const_cast<AddShardCoordinator*>(this)->getCompletionFuture().get(opCtx);
    invariant(_result.is_initialized());
    return *_result;
}

// TODO (SPM-4017): these changes should be done on the cluster command level.
void AddShardCoordinator::_verifyInput() const {
    uassert(ErrorCodes::BadValue, "Invalid connection string", _doc.getConnectionString());

    if (_doc.getConnectionString().type() != ConnectionString::ConnectionType::kStandalone &&
        _doc.getConnectionString().type() != ConnectionString::ConnectionType::kReplicaSet) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Invalid connection string "
                                << _doc.getConnectionString().toString());
    }

    uassert(ErrorCodes::BadValue,
            "shard name cannot be empty",
            !_doc.getProposedName() || !_doc.getProposedName()->empty());
}

}  // namespace mongo
