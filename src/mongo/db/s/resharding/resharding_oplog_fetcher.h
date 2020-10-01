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

#include "mongo/base/status_with.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/uuid.h"

namespace mongo {
class ReshardingOplogFetcher {
public:
    /**
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
    ReshardingDonorOplogId iterate(OperationContext* opCtx,
                                   DBClientBase* conn,
                                   boost::intrusive_ptr<ExpressionContext> expCtx,
                                   ReshardingDonorOplogId startAfter,
                                   UUID collUUID,
                                   const ShardId& recipientShard,
                                   bool doesDonorOwnMinKeyChunk,
                                   NamespaceString toWriteInto);

private:
};
}  // namespace mongo
