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

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/util/future.h"

namespace mongo {

class OperationContext;

/**
 * Iterator for extracting oplog entries from the resharding donor oplog buffer. This is not thread
 * safe.
 */
class ReshardingDonorOplogIterator {
public:
    ReshardingDonorOplogIterator(NamespaceString donorOplogBufferNs,
                                 boost::optional<ReshardingDonorOplogId> resumeToken);

    /**
     * Returns the next oplog entry. Returns boost::none when there are no more entries to return.
     * Calling getNext() when the previously returned future is not ready is undefined.
     */
    Future<boost::optional<repl::OplogEntry>> getNext(OperationContext* opCtx);

    /**
     * Returns false if this iterator has seen the final oplog entry. Since this is not thread safe,
     * should not be called while there is a pending future from getNext() that is not ready.
     */
    bool hasMore() const;

private:
    /**
     * Returns a future to wait until a new oplog entry is inserted to the target oplog collection.
     */
    Future<void> _waitForNewOplog();

    /**
     * Creates a new expression context that can be used to make a new pipeline to query the target
     * oplog collection.
     */
    boost::intrusive_ptr<ExpressionContext> _makeExpressionContext(OperationContext* opCtx);

    const NamespaceString _oplogBufferNs;

    boost::optional<ReshardingDonorOplogId> _resumeToken;

    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
    bool _hasSeenFinalOplogEntry{false};
};

}  // namespace mongo
