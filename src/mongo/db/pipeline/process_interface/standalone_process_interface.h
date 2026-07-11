// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/process_interface/non_shardsvr_process_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

namespace mongo {

/**
 * Process interface intended to be used for mongod servers configured as standalones.
 */
class StandaloneProcessInterface : public NonShardServerProcessInterface {
public:
    StandaloneProcessInterface(std::shared_ptr<executor::TaskExecutor> exec)
        : NonShardServerProcessInterface(std::move(exec)) {}

    std::unique_ptr<MongoProcessInterface::WriteSizeEstimator> getWriteSizeEstimator(
        OperationContext* opCtx, const NamespaceString& ns) const final {
        return std::make_unique<LocalWriteSizeEstimator>();
    }

    ~StandaloneProcessInterface() override = default;
};

}  // namespace mongo
