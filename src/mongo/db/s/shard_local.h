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

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/rs_local_client.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class ShardLocal : public Shard {
    ShardLocal(const ShardLocal&) = delete;
    ShardLocal& operator=(const ShardLocal&) = delete;

public:
    // ShardLocal doesn't have a "real" connection string since it's always a connection to the
    // local process, so it uses the hardcoded local connection string. Only used in tests.
    static const ConnectionString kLocalConnectionString;

    explicit ShardLocal(const ShardId& id);

    ~ShardLocal() = default;

    /**
     * These functions are implemented for the Shard interface's sake. They should not be called on
     * ShardLocal because doing so triggers invariants.
     */
    const ConnectionString& getConnString() const override;
    std::shared_ptr<RemoteCommandTargeter> getTargeter() const override;
    void updateReplSetMonitor(const HostAndPort& remoteHost,
                              const Status& remoteCommandStatus) override;

    std::string toString() const override;

    bool isRetriableError(ErrorCodes::Error code, RetryPolicy options) final;

    void runFireAndForgetCommand(OperationContext* opCtx,
                                 const ReadPreferenceSetting& readPref,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) override;

    Status runAggregation(
        OperationContext* opCtx,
        const AggregateCommandRequest& aggRequest,
        std::function<bool(const std::vector<BSONObj>& batch,
                           const boost::optional<BSONObj>& postBatchResumeToken)> callback);

private:
    StatusWith<Shard::CommandResponse> _runCommand(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& unused,
                                                   const DatabaseName& dbName,
                                                   Milliseconds maxTimeMSOverrideUnused,
                                                   const BSONObj& cmdObj) final;

    StatusWith<Shard::QueryResponse> _runExhaustiveCursorCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const DatabaseName& dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj) final;

    StatusWith<Shard::QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcernLevel,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none) final;

    RSLocalClient _rsLocalClient;
};

}  // namespace mongo
