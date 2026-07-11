// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

class CurrentOpCommandBase : public BasicCommand {
public:
    CurrentOpCommandBase() : BasicCommand("currentOp") {}

    ~CurrentOpCommandBase() override {}

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    bool allowsAfterClusterTime(const BSONObj& cmdObj) const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return true;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

private:
    /**
     * Adds or removes stages before the final $group stage is appended to the pipeline.
     */
    virtual void modifyPipeline(std::vector<BSONObj>* pipeline) const {};

    /**
     * Runs the aggregation specified by the supplied AggregateCommandRequest, returning a
     * CursorResponse if successful or a Status containing the error otherwise.
     */
    virtual StatusWith<CursorResponse> runAggregation(OperationContext* opCtx,
                                                      AggregateCommandRequest& request) const = 0;

    /**
     * Allows overriders to optionally write additional data to the response object before the final
     * 'ok' field is added.
     */
    virtual void appendToResponse(BSONObjBuilder* result) const {};
};

}  // namespace mongo
