/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/util/future.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace repl {

class DurableOplogEntry;
class OplogEntry;

}  // namespace repl

class OperationContext;

/**
 * Updates this shard's config.transactions table based on oplog entries for retryable writes and
 * multi-statement transactions that already executed on some donor shard.
 *
 * Instances of this class are thread-safe.
 */
class ReshardingOplogSessionApplication {
public:
    ReshardingOplogSessionApplication(NamespaceString oplogBufferNss);

    /**
     * Returns boost::none if the operation was successfully applied.
     *
     * If the operation couldn't be applied due to a prepared transaction, then this function
     * returns a future to wait on before attempting to apply the operation again.
     */
    boost::optional<SharedSemiFuture<void>> tryApplyOperation(OperationContext* opCtx,
                                                              const repl::OplogEntry& op) const;

private:
    /**
     * Fetches the pre or post image oplog entry for the oplog entry with the given resharding id
     * from the oplog buffer collection, and writes a no-op oplog entry to store the pre/post image
     * and returns its op time. If no pre or post image oplog entry is found, skips the write and
     * returns none.
     */
    boost::optional<repl::OpTime> _logPrePostImage(OperationContext* opCtx,
                                                   const ReshardingDonorOplogId& opId,
                                                   const repl::OpTime& prePostImageOpTime) const;

    const NamespaceString _oplogBufferNss;
};

}  // namespace mongo
