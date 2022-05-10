/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/reshard_collection_coordinator_document_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/util/future.h"

namespace mongo {
class ReshardCollectionCoordinator : public ShardingDDLCoordinator {
public:
    using StateDoc = ReshardCollectionCoordinatorDocument;
    using Phase = ReshardCollectionCoordinatorPhaseEnum;

    ReshardCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                 const BSONObj& initialState);

    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

protected:
    ReshardCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                 const BSONObj& initialState,
                                 bool persistCoordinatorDocument);

private:
    ShardingDDLCoordinatorMetadata const& metadata() const override {
        stdx::lock_guard l{_docMutex};
        return _doc.getShardingDDLCoordinatorMetadata();
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    template <typename Func>
    auto _executePhase(const Phase& newPhase, Func&& func) {
        return [=] {
            const auto& currPhase = _doc.getPhase();

            if (currPhase > newPhase) {
                // Do not execute this phase if we already reached a subsequent one.
                return;
            }
            if (currPhase < newPhase) {
                // Persist the new phase if this is the first time we are executing it.
                _enterPhase(newPhase);
            }
            return func();
        };
    }

    void _enterPhase(Phase newPhase);

    const BSONObj _initialState;
    mutable Mutex _docMutex = MONGO_MAKE_LATCH("ReshardCollectionCoordinator::_docMutex");
    ReshardCollectionCoordinatorDocument _doc;

    const mongo::ReshardCollectionRequest _request;

    const bool _persistCoordinatorDocument;  // TODO: SERVER-62338 remove this then 6.0 branches out
};

// TODO: SERVER-62338 remove this then 6.0 branches out
class ReshardCollectionCoordinator_NORESILIENT : public ReshardCollectionCoordinator {
public:
    ReshardCollectionCoordinator_NORESILIENT(ShardingDDLCoordinatorService* service,
                                             const BSONObj& initialState);
};

}  // namespace mongo
