// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/topology/user_write_block/set_user_write_block_mode_coordinator_document_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PARENT_PRIVATE]] SetUserWriteBlockModeCoordinator
    : public ConfigsvrCoordinatorImpl<SetUserWriteBlockModeCoordinatorDocument,
                                      SetUserWriteBlockModeCoordinatorPhaseEnum> {
public:
    using StateDoc = SetUserWriteBlockModeCoordinatorDocument;
    using Phase = SetUserWriteBlockModeCoordinatorPhaseEnum;

    explicit SetUserWriteBlockModeCoordinator(const BSONObj& stateDoc)
        : ConfigsvrCoordinatorImpl(stateDoc) {}

    bool hasSameOptions(const BSONObj& participantDoc) const override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override {}

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    const ConfigsvrCoordinatorMetadata& metadata() const override;

    std::string_view serializePhase(const Phase& phase) const override {
        return idl::serialize(phase);
    }
};

}  // namespace mongo
