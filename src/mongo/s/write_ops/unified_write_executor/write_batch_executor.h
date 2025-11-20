/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/request_types/coordinate_multi_update_gen.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/s/write_ops/wc_error.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

using CommandReplyVariant =
    std::variant<BulkWriteCommandReply, write_ops::FindAndModifyCommandReply>;

class BasicResponse {
public:
    BasicResponse(boost::optional<StatusWith<CommandReplyVariant>> swReply,
                  boost::optional<WriteConcernErrorDetail> wce,
                  std::vector<WriteOp> ops,
                  bool transientTxnError,
                  bool errorsOnly,
                  boost::optional<HostAndPort> hostAndPort = boost::none)
        : _swReply(std::move(swReply)),
          _wce(std::move(wce)),
          _ops(std::move(ops)),
          _transientTxnError(transientTxnError),
          _errorsOnly(errorsOnly),
          _hostAndPort(std::move(hostAndPort)) {}

    bool isOK() const {
        return _swReply && _swReply->isOK();
    }
    bool isError() const {
        return _swReply && !_swReply->isOK();
    }
    bool isEmpty() const {
        return !_swReply;
    }

    // Returns the status of this ShardResponse. This method may only be called when either 'isOK()'
    // is true or 'isError()' is true.
    const Status& getStatus() const;

    // Helper methods for accessing the CommandReplyVariant object. These methods may only be
    // called when 'isOK()' is true.
    CommandReplyVariant& getReply();
    const CommandReplyVariant& getReply() const;

    const boost::optional<WriteConcernErrorDetail>& getWriteConcernError() const {
        return _wce;
    }

    const std::vector<WriteOp>& getOps() const {
        return _ops;
    }

    // Returns true if this ShardResponse contains a transient transaction error, otherwise returns
    // false.
    bool hasTransientTxnError() const {
        return isError() && _transientTxnError;
    }

    // Returns the transient transaction error contained within this NoRetryWriteBatchResponse.
    // This method may only be called if hasTransientTxnError() is true.
    const Status& getTransientTxnError() const {
        tassert(11272106, "Expected transient transaction error", hasTransientTxnError());
        return getStatus();
    }

    bool getErrorsOnly() const {
        return _errorsOnly;
    }

    const boost::optional<HostAndPort>& getHostAndPort() const {
        return _hostAndPort;
    }

    // Returns true if this ShardResponse contains a WouldChangeOwningShard error.
    bool isWouldChangeOwningShardError() const {
        return isError() && getStatus() == ErrorCodes::WouldChangeOwningShard;
    }

    bool isShutdownError() const;

private:
    // This field may hold either a CommandReplyVariant, an error, or boost::none.
    boost::optional<StatusWith<CommandReplyVariant>> _swReply;

    // This field holds the write concern error (if one occurred).
    boost::optional<WriteConcernErrorDetail> _wce;

    // Ops referencing the original command from the client in the order they were specified in the
    // command to the shard. The items in this array should appear in the order you would see them
    // in the reply item's of a bulk write so that response.ops[replyItem.getIdx()] should return
    // the corresponding WriteOp from the original command from the client.
    std::vector<WriteOp> _ops;

    // This field indicates if 'swReply' contains a transient transaction error.
    bool _transientTxnError = false;

    // This flag indicates if the shard request had the 'errorsOnly' parameter set to true.
    bool _errorsOnly = false;

    // For debugging purposes.
    boost::optional<HostAndPort> _hostAndPort;
};

class ShardResponse : public BasicResponse {
public:
    using BasicResponse::BasicResponse;

    /**
     * Creates a ShardResponse from the supplied RemoteCommandResponse.
     */
    static ShardResponse make(StatusWith<executor::RemoteCommandResponse> swResponse,
                              std::vector<WriteOp> ops,
                              bool inTransaction = false,
                              bool errorsOnly = false,
                              boost::optional<HostAndPort> hostAndPort = boost::none,
                              boost::optional<const ShardId&> shardId = boost::none);

    /**
     * Creates an "empty" ShardResponse. This method is used when there is no RemoteCommandResponse
     * for a given 'shardId' (in cases where we decided to break out of the ARS loop early).
     */
    static ShardResponse makeEmpty(std::vector<WriteOp> ops, bool errorsOnly = false);
};

struct EmptyBatchResponse {};

struct SimpleWriteBatchResponse {
    std::vector<std::pair<ShardId, ShardResponse>> shardResponses;
};

class NoRetryWriteBatchResponse : public BasicResponse {
public:
    using BasicResponse::BasicResponse;

    /**
     * Creates a NoRetryWriteBatchResponse from the supplied BSONObj response.
     */
    static NoRetryWriteBatchResponse make(const StatusWith<BSONObj>& swResponse,
                                          boost::optional<WriteConcernErrorDetail> wce,
                                          const WriteOp& op,
                                          bool inTransaction,
                                          bool errorsOnly);

    const WriteOp& getOp() const {
        tassert(11182200, "Expected vector to contain exactly one op", getOps().size() == 1);
        return getOps().front();
    }
};

using WriteBatchResponse =
    std::variant<EmptyBatchResponse, SimpleWriteBatchResponse, NoRetryWriteBatchResponse>;

class WriteBatchExecutor {
public:
    /**
     * Returns true if the scheduler must provide a RoutingContext when executing the specified
     * batch, otherwise returns false.
     *
     * The scheduler uses this method to determine if it needs to provide a RoutingContext when
     * it calls execute().
     */
    WriteBatchExecutor(WriteCommandRef cmdRef, BSONObj originalCmdObj = BSONObj())
        : _cmdRef(cmdRef), _originalCmdObj(originalCmdObj) {}

    bool usesProvidedRoutingContext(const WriteBatch& batch) const;

    /**
     * This method executes the specified 'batch' and returns the responses for the batch.
     */
    WriteBatchResponse execute(OperationContext* opCtx,
                               RoutingContext& routingCtx,
                               const WriteBatch& batch);

private:
    struct IsEmbeddedCommand {
        enum Value : bool { No = false, Yes = true };
        Value value;
        constexpr IsEmbeddedCommand(Value v) : value(v) {}
        constexpr explicit operator bool() const {
            return static_cast<bool>(value);
        }
    };
    struct ShouldAppendLsidAndTxnNumber {
        enum Value : bool { No = false, Yes = true };
        Value value;
        constexpr ShouldAppendLsidAndTxnNumber(Value v) : value(v) {}
        constexpr explicit operator bool() const {
            return static_cast<bool>(value);
        }
    };
    struct ShouldAppendReadWriteConcern {
        enum Value : bool { No = false, Yes = true };
        Value value;
        constexpr ShouldAppendReadWriteConcern(Value v) : value(v) {}
        constexpr explicit operator bool() const {
            return static_cast<bool>(value);
        }
    };

    WriteBatchResponse _execute(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const EmptyBatch& batch);

    WriteBatchResponse _execute(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const SimpleWriteBatch& batch);

    WriteBatchResponse _execute(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const TwoPhaseWriteBatch& batch);

    WriteBatchResponse _execute(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const InternalTransactionBatch& batch);

    WriteBatchResponse _execute(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                const MultiWriteBlockingMigrationsBatch& batch);

    BulkWriteCommandRequest buildBulkWriteRequestWithoutTxnInfo(
        OperationContext* opCtx,
        const std::vector<WriteOp>& ops,
        const std::map<NamespaceString, ShardEndpoint>& versionByNss,
        const std::set<NamespaceString>& nssIsViewfulTimeseries,
        const std::map<WriteOpId, UUID>& sampleIds,
        bool errorsOnly,
        boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
        IsEmbeddedCommand isEmbeddedCommand) const;

    BSONObj buildBulkWriteRequest(
        OperationContext* opCtx,
        const std::vector<WriteOp>& ops,
        const std::map<NamespaceString, ShardEndpoint>& versionByNss,
        const std::set<NamespaceString>& nssIsViewfulTimeseries,
        const std::map<WriteOpId, UUID>& sampleIds,
        bool errorsOnly,
        boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
        IsEmbeddedCommand isEmbeddedCommand,
        ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
        ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const;

    BSONObj buildFindAndModifyRequest(
        OperationContext* opCtx,
        const std::vector<WriteOp>& ops,
        const std::map<NamespaceString, ShardEndpoint>& versionByNss,
        const std::set<NamespaceString>& nssIsViewfulTimeseries,
        const std::map<WriteOpId, UUID>& sampleIds,
        boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
        IsEmbeddedCommand isEmbeddedCommand,
        ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
        ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const;

    BSONObj buildRequest(OperationContext* opCtx,
                         const std::vector<WriteOp>& ops,
                         const std::map<NamespaceString, ShardEndpoint>& versionByNss,
                         const std::set<NamespaceString>& nssIsViewfulTimeseries,
                         const std::map<WriteOpId, UUID>& sampleIds,
                         bool errorsOnly,
                         boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                         IsEmbeddedCommand isEmbeddedCommand,
                         ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
                         ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const;

    bool getErrorsOnlyForShardRequest(const std::vector<WriteOp>& ops) const;

    const WriteCommandRef _cmdRef;
    const BSONObj _originalCmdObj;
};

}  // namespace unified_write_executor
}  // namespace mongo
