// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/find_key.h"

namespace mongo::query_stats {

void FindCmdComponents::appendTo(BSONObjBuilder& bob,
                                 const query_shape::SerializationOptions& opts) const {

    if (_hasField.allowPartialResults) {
        bob.append(FindCommandRequest::kAllowPartialResultsFieldName, _allowPartialResults);
    }

    // Fields for literal redaction. Adds batchSize, and noCursorTimeOut.

    if (_hasField.noCursorTimeout) {
        bob.append(FindCommandRequest::kNoCursorTimeoutFieldName, _noCursorTimeout);
    }

    // We don't store the specified batch size value since it doesn't matter.
    // Provide an arbitrary literal long here.
    tassert(7973602,
            "Serialization policy not supported - original values have been discarded",
            !opts.isKeepingLiteralsUnchanged());

    if (_hasField.batchSize) {
        opts.appendLiteral(&bob, FindCommandRequest::kBatchSizeFieldName, 0ll);
    }
}

}  // namespace mongo::query_stats
