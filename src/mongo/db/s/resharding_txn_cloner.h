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

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/shard_id.h"

namespace mongo {


/**
 * Create pipeline stages for iterating donor config.transactions.  The pipeline has these stages:
 * pipeline: [
 *      {$match: {_id: {$gt: <startAfter>}, state: {$exists: false}}},
 *      {$sort: {_id: 1}},
 *      {$match: {"lastWriteOpTime.ts": {$lt: <fetchTimestamp>}}},
 * ],
 * Note that the caller is responsible for making sure that the transactions ns is set in the
 * expCtx.
 *
 * fetchTimestamp never isNull()
 */
std::unique_ptr<Pipeline, PipelineDeleter> createConfigTxnCloningPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter);

/**
 * Clone config.transactions from source and updates the config.transactions on itself.
 * The parameter merge is a function called on every transaction received and should be used
 * to merge the transaction into this machine's own congig.transactions collection.
 *
 * returns a pointer to the fetcher object sending the command.
 */
std::unique_ptr<Fetcher> cloneConfigTxnsForResharding(
    OperationContext* opCtx,
    const ShardId& shardId,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter,
    std::function<void(OperationContext*, BSONObj)> merge,
    Status* status);

/**
 * Callback function to be used to merge transactions cloned by cloneConfigTxnsForResharding
 */
void configTxnsMergerForResharding(OperationContext* opCtx, BSONObj donorBsonTransaction);

}  // namespace mongo
