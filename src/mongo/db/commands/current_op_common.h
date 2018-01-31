/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/query/cursor_response.h"

namespace mongo {

class CurrentOpCommandBase : public BasicCommand {
public:
    CurrentOpCommandBase() : BasicCommand("currentOp") {}

    virtual ~CurrentOpCommandBase() {}

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    bool allowsAfterClusterTime(const BSONObj& cmdObj) const final {
        return false;
    }

    bool slaveOk() const final {
        return true;
    }

    bool adminOnly() const final {
        return true;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

private:
    /**
     * Adds or removes stages before the final $group stage is appended to the pipeline.
     */
    virtual void modifyPipeline(std::vector<BSONObj>* pipeline) const {};

    /**
     * Runs the aggregation specified by the supplied AggregationRequest, returning a CursorResponse
     * if successful or a Status containing the error otherwise.
     */
    virtual StatusWith<CursorResponse> runAggregation(OperationContext* opCtx,
                                                      const AggregationRequest& request) const = 0;

    /**
     * Allows overriders to optionally write additional data to the response object before the final
     * 'ok' field is added.
     */
    virtual void appendToResponse(BSONObjBuilder* result) const {};
};

}  // namespace mongo
