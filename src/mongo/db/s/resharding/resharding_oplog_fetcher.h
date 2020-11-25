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
#pragma once

#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/background.h"
#include "mongo/util/uuid.h"

namespace mongo {
class ReshardingOplogFetcher {
public:
    ReshardingOplogFetcher(UUID reshardingUUID,
                           UUID collUUID,
                           ReshardingDonorOplogId startAt,
                           ShardId donorShard,
                           ShardId recipientShard,
                           bool doesDonorOwnMinKeyChunk,
                           NamespaceString toWriteInto);

    /**
     * Schedules a task that will do the following:
     *
     * - Find a valid connection to fetch oplog entries from.
     * - Send an aggregation request + getMores until either:
     * -- The "final resharding" oplog entry is found.
     * -- An interruption occurs.
     * -- A different error occurs.
     *
     * In the first two circumstances, the task will return. In the last circumstance, the task will
     * be rescheduled in a way that resumes where it had left off from.
     */
    void schedule(executor::TaskExecutor* exector);

    /**
     * Given a connection, fetches and copies oplog entries until reaching an error, or coming
     * across a sentinel finish oplog entry. Throws if there's more oplog entries to be copied.
     */
    void consume(DBClientBase* conn);

    /**
     * Kill the underlying client the BackgroundJob is using to expedite cleaning up resources when
     * the output is no longer necessary. The underlying `toWriteInto` collection is left intact,
     * though likely incomplete.
     */
    void setKilled();

    /**
     * Returns boost::none if the last oplog entry to be copied is found. Otherwise returns the
     * ReshardingDonorOplogId to resume querying from.
     *
     * Issues an aggregation to `DBClientBase`s starting at `startAfter` and copies the entries
     * relevant to `recipientShard` into `toWriteInto`. Control is returned when the aggregation
     * cursor is exhausted.
     *
     * Returns an identifier for the last oplog-ish document written to `toWriteInto`.
     *
     * This method throws.
     *
     * TODO SERVER-51245 Replace `DBClientBase` with a `Shard`. Right now `Shard` does not do things
     * like perform aggregate commands nor does it expose a cursor/stream interface. However, using
     * a `Shard` object will provide critical behavior such as advancing logical clock values on a
     * response and targetting a node to send the aggregation command to.
     */
    boost::optional<ReshardingDonorOplogId> iterate(OperationContext* opCtx,
                                                    DBClientBase* conn,
                                                    boost::intrusive_ptr<ExpressionContext> expCtx,
                                                    ReshardingDonorOplogId startAfter,
                                                    UUID collUUID,
                                                    const ShardId& recipientShard,
                                                    bool doesDonorOwnMinKeyChunk,
                                                    NamespaceString toWriteInto);

    int getNumOplogEntriesCopied() {
        return _numOplogEntriesCopied;
    }

private:
    /**
     * Returns true if there's more work to do and the task should be rescheduled.
     */
    bool _runTask();

    const UUID _reshardingUUID;
    const UUID _collUUID;
    ReshardingDonorOplogId _startAt;
    const ShardId _donorShard;
    const ShardId _recipientShard;
    const bool _doesDonorOwnMinKeyChunk;
    const NamespaceString _toWriteInto;

    ServiceContext::UniqueClient _client;
    AtomicWord<bool> _isAlive{true};

    int _numOplogEntriesCopied = 0;
};
}  // namespace mongo
