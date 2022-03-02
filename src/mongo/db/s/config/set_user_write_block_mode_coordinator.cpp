/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/set_user_write_block_mode_coordinator.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {

bool SetUserWriteBlockModeCoordinator::hasSameOptions(const BSONObj& otherDocBSON) const {
    const auto otherDoc = StateDoc::parse(
        IDLParserErrorContext("SetUserWriteBlockModeCoordinatorDocument"), otherDocBSON);

    return _doc.getBlock() == otherDoc.getBlock();
}

boost::optional<BSONObj> SetUserWriteBlockModeCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "SetUserWriteBlockModeCoordinator");
    bob.append("op", "command");
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void SetUserWriteBlockModeCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(
        6347305,
        2,
        "SetUserWriteBlockModeCoordinator phase transition",
        "newPhase"_attr = SetUserWriteBlockModeCoordinatorPhase_serializer(newDoc.getPhase()),
        "oldPhase"_attr = SetUserWriteBlockModeCoordinatorPhase_serializer(_doc.getPhase()));

    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<StateDoc> store(NamespaceString::kConfigsvrCoordinatorsNamespace);

    if (_doc.getPhase() == Phase::kUnset) {
        store.add(opCtx.get(), newDoc, WriteConcerns::kMajorityWriteConcernShardingTimeout);
    } else {
        store.update(opCtx.get(),
                     BSON(StateDoc::kIdFieldName << _coordId.toBSON()),
                     newDoc.toBSON(),
                     WriteConcerns::kMajorityWriteConcernNoTimeout);
    }

    _doc = std::move(newDoc);
}

ExecutorFuture<void> SetUserWriteBlockModeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(Phase::kSetUserWriteBlockMode, [this, anchor = shared_from_this()] {
            // TODO Implement
        }));
}

}  // namespace mongo
