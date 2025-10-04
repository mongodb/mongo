/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/mr_common.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/modules.h"

namespace mongo {

class MapReduceCommandBase : public BasicCommand {
public:
    MapReduceCommandBase() : BasicCommand("mapReduce", "mapreduce") {}

    std::string help() const override {
        return "Runs the mapReduce command. See http://dochub.mongodb.org/core/mapreduce for "
               "details.";
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    /**
     * The mapReduce command supports only 'local' and 'available' readConcern levels.
     * For aggregation-based mapReduce there are no known restrictions to broader support, but work
     * would need to be done confirm support for both command and aggregation stages as is done for
     * the aggregate command.
     */
    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        static const Status kReadConcernNotSupported{ErrorCodes::InvalidOptions,
                                                     "read concern not supported"};
        static const Status kDefaultReadConcernNotPermitted{ErrorCodes::InvalidOptions,
                                                            "default read concern not permitted"};
        return {{level != repl::ReadConcernLevel::kLocalReadConcern &&
                     level != repl::ReadConcernLevel::kAvailableReadConcern,
                 kReadConcernNotSupported},
                {kDefaultReadConcernNotPermitted}};
    }

    bool shouldAffectReadOptionCounters() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return map_reduce_common::mrSupportsWriteConcern(cmd);
    }

    bool allowsAfterClusterTime(const BSONObj& cmd) const override {
        return false;
    }

    bool canIgnorePrepareConflicts() const override {
        // Map-Reduce is a special case for prepare conflicts. It may do writes to an output
        // collection, but it enables enforcement of prepare conflicts before doing so. See use of
        // EnforcePrepareConflictsBlock.
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        return map_reduce_common::checkAuthForMapReduce(this, opCtx, dbName, cmdObj);
    }

    virtual void _explainImpl(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmd,
                              BSONObjBuilder& result,
                              boost::optional<ExplainOptions::Verbosity> verbosity) const = 0;

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        auto builder = result->getBodyBuilder();
        auto explain = boost::make_optional(verbosity);
        try {
            _explainImpl(opCtx, request.parseDbName(), request.body, builder, explain);
        } catch (...) {
            return exceptionToStatus();
        }
        return Status::OK();
    }
};

}  // namespace mongo
