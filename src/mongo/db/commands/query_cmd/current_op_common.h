/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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
