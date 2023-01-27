/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {
namespace analyze_shard_key {

// The size limit for the documents to an insert in a single batch. Leave some padding for other
// fields in the insert command.
constexpr int kMaxBSONObjSizePerInsertBatch = BSONObjMaxUserSize - 100 * 1024;

/*
 * Returns the percentage between 'part' and 'whole' (between 0 and 100).
 */
double calculatePercentage(double part, double whole);

/**
 * Runs the aggregate command 'aggRequest' and applies 'callbackFn' to each returned document. On a
 * sharded cluster, automatically retries on shard versioning errors. Does not support runnning
 * getMore commands for the aggregation.
 */
void runAggregate(OperationContext* opCtx,
                  AggregateCommandRequest aggRequest,
                  std::function<void(const BSONObj&)> callbackFn);

/**
 * Same as above, but on a sharded cluster, targets all the shards that owns the collection 'nss'
 * instead.
 */
void runAggregate(OperationContext* opCtx,
                  const NamespaceString& nss,
                  AggregateCommandRequest aggRequest,
                  std::function<void(const BSONObj&)> callbackFn);

/*
 * Inserts the documents 'docs' into the collection 'nss'. If this mongod is currently the primary,
 * runs the insert command locally. Otherwise, runs the command on the remote primary. Internally
 * asserts that the top-level command is OK, then asserts the write status using the
 * 'uassertWriteStatusFn' callback. Internally retries the write command on retryable errors.
 */
void insertDocuments(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const std::vector<BSONObj>& docs,
                     const std::function<void(const BSONObj&)>& uassertWriteStatusFn);

/*
 * Drops the collection 'nss'. If this mongod is currently the primary, runs the dropCollection
 * command locally. Otherwise, runs the command on the remote primary. Internally asserts that the
 * top-level command is OK or the error is NamespaceNotFound. Internally retries the write command
 * on retryable errors.
 */
void dropCollection(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace analyze_shard_key
}  // namespace mongo
