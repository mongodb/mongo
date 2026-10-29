// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

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
