/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {

ReshardingCoordinatorService::ReshardingCoordinator::ReshardingCoordinator(const BSONObj& state)
    : PrimaryOnlyService::TypedInstance<ReshardingCoordinator>(),
      _id(state["_id"].wrap().getOwned()),
      _stateDoc(ReshardingCoordinatorDocument::parse(
          IDLParserErrorContext("ReshardingCoordinatorStateDoc"), state)) {
    _reshardingCoordinatorObserver = std::make_shared<ReshardingCoordinatorObserver>();
}

ReshardingCoordinatorService::ReshardingCoordinator::~ReshardingCoordinator() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_initialChunksAndZonesPromise.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

void ReshardingCoordinatorService::ReshardingCoordinator::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    ExecutorFuture<void>(**executor)
        .then([this, executor] { return _init(executor); })
        .then([this] { _tellAllRecipientsToRefresh(); })
        .then([this, executor] { return _awaitAllRecipientsCreatedCollection(executor); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
        .then([this] { _tellAllRecipientsToRefresh(); })
        .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .then([this, executor] { return _awaitAllRecipientsInStrictConsistency(executor); })
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            return _commit(updatedStateDoc);
        })
        .then([this] {
            if (_stateDoc.getState() > CoordinatorStateEnum::kRenaming) {
                return;
            }

            this->_runUpdates(CoordinatorStateEnum::kRenaming, _stateDoc);
            return;
        })
        .then([this, executor] { return _awaitAllRecipientsRenamedCollection(executor); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .then([this, executor] { return _awaitAllDonorsDroppedOriginalCollection(executor); })
        .then([this] { _tellAllRecipientsToRefresh(); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .onError([this](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return status;
            }

            _runUpdates(CoordinatorStateEnum::kError, _stateDoc);

            LOGV2(4956902,
                  "Resharding failed",
                  "namespace"_attr = _stateDoc.getNss().ns(),
                  "newShardKeyPattern"_attr = _stateDoc.getReshardingKey(),
                  "error"_attr = status);

            // TODO wait for donors and recipients to abort the operation and clean up state
            _tellAllRecipientsToRefresh();
            _tellAllDonorsToRefresh();

            return status;
        })
        .getAsync([this](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return;
            }

            if (status.isOK()) {
                _completionPromise.emplaceValue();
            } else {
                _completionPromise.setError(status);
            }
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_initialChunksAndZonesPromise.getFuture().isReady()) {
        _initialChunksAndZonesPromise.setError(status);
    }

    _reshardingCoordinatorObserver->interrupt(status);

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void ReshardingCoordinatorService::ReshardingCoordinator::setInitialChunksAndZones(
    std::vector<ChunkType> initialChunks, std::vector<TagsType> newZones) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_stateDoc.getState() > CoordinatorStateEnum::kInitializing ||
        _initialChunksAndZonesPromise.getFuture().isReady()) {
        return;
    }

    _initialChunksAndZonesPromise.emplaceValue(
        ChunksAndZones{std::move(initialChunks), std::move(newZones)});
}

std::shared_ptr<ReshardingCoordinatorObserver>
ReshardingCoordinatorService::ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

ExecutorFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::_init(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _initialChunksAndZonesPromise.getFuture()
        .thenRunOn(**executor)
        .then([this](const ChunksAndZones& initialChunksAndZones) {
            // TODO SERVER-50304 Run this insert in a transaction with writes to config.collections,
            // config.chunks, and config.tags
            auto opCtx = cc().makeOperationContext();
            DBDirectClient client(opCtx.get());

            const auto commandResponse = client.runCommand([&] {
                write_ops::Insert insertOp(NamespaceString::kConfigReshardingOperationsNamespace);
                insertOp.setDocuments({_stateDoc.toBSON()});
                return insertOp.serialize({});
            }());
            uassertStatusOK(getStatusFromWriteCommandReply(commandResponse->getCommandReply()));

            invariant(_stateDoc.getState() == CoordinatorStateEnum::kInitializing);
            _stateDoc.setState(CoordinatorStateEnum::kInitialized);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsCreatedCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kInitialized) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsCreatedCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kPreparingToDonate, updatedStateDoc);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllDonorsReadyToDonate()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            // TODO SERVER-49573 Calculate the fetchTimestamp from the updatedStateDoc then pass it
            // into _runUpdates.
            this->_runUpdates(CoordinatorStateEnum::kCloning, updatedStateDoc);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kMirroring, updatedStateDoc);
        });
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kMirroring) {
        // If in recovery, just return the existing _stateDoc.
        return _stateDoc;
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency();
}

Future<void> ReshardingCoordinatorService::ReshardingCoordinator::_commit(
    ReshardingCoordinatorDocument updatedStateDoc) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kMirroring) {
        return Status::OK();
    }

    // TODO SERVER-50304 Run this update in a transaction with writes to config.collections,
    // config.chunks, and config.tags
    this->_runUpdates(CoordinatorStateEnum::kCommitted, updatedStateDoc);

    return Status::OK();
};

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsRenamedCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kRenaming) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsRenamedCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kDropping, updatedStateDoc);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsDroppedOriginalCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_stateDoc.getState() > CoordinatorStateEnum::kDropping) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllDonorsDroppedOriginalCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kDone, updatedStateDoc);
        });
}

// TODO SERVER-50304 Run this write in a transaction with updates to config.collections (and
// the initial chunks to config.chunks and config.tags if nextState is kInitialized)
void ReshardingCoordinatorService::ReshardingCoordinator::_runUpdates(
    CoordinatorStateEnum nextState,
    ReshardingCoordinatorDocument updatedStateDoc,
    boost::optional<Timestamp> fetchTimestamp) {
    // Build new state doc for update
    updatedStateDoc.setState(nextState);
    if (fetchTimestamp) {
        auto fetchTimestampStruct = updatedStateDoc.getFetchTimestampStruct();
        if (fetchTimestampStruct.getFetchTimestamp())
            invariant(fetchTimestampStruct.getFetchTimestamp().get() == fetchTimestamp.get());

        fetchTimestampStruct.setFetchTimestamp(std::move(fetchTimestamp));
    }

    // Run update
    auto opCtx = cc().makeOperationContext();
    DBDirectClient client(opCtx.get());

    const auto commandResponse = client.runCommand([&] {
        write_ops::Update updateOp(NamespaceString::kConfigReshardingOperationsNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(_id);
            entry.setU(
                write_ops::UpdateModification::parseFromClassicUpdate(updatedStateDoc.toBSON()));
            return entry;
        }()});
        return updateOp.serialize(
            BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    // Throw if the update did not match a document. This means the state doc was removed out from
    // under the operation.
    uassert(495690,
            str::stream() << "Found that the resharding coordinator state document is missing when "
                             "attempting to update state for namespace "
                          << _stateDoc.getNss().ns(),
            commandReply.getIntField("n") == 1);

    // Update in-memory state doc
    _stateDoc = updatedStateDoc;
}

// TODO
void ReshardingCoordinatorService::ReshardingCoordinator::
    _markCoordinatorStateDocAsGarbageCollectable() {}

// TODO
void ReshardingCoordinatorService::ReshardingCoordinator::_removeReshardingFields() {}

// TODO
void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllRecipientsToRefresh() {}

// TODO
void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllDonorsToRefresh() {}

}  // namespace mongo
