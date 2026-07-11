// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/client_cursor/cursor_idl_validator.h"

#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/util/assert_util.h"

#include <boost/none.hpp>


namespace mongo {

/**
 * Function used by the IDL parser to validate that a response has exactly one cursor type field.
 */
void validateIDLParsedCursorResponse(const CursorInitialReply* idlParsedObj) {
    bool hasCursor = idlParsedObj->getCursor() != boost::none;
    bool hasCursors = idlParsedObj->getCursors() != boost::none;
    uassert(6253507,
            "MultiResponseInitialCursor must have exactly one of 'cursor' or 'cursors' fields",
            hasCursor != hasCursors);
}

/**
 * Function used by the IDL parser to verify that a response cursor has a firstBatch or nextBatch.
 */
void validateIDLParsedAnyCursor(const AnyCursor* idlParsedObj) {
    bool hasFirst = idlParsedObj->getFirstBatch() != boost::none;
    bool hasNext = idlParsedObj->getNextBatch() != boost::none;
    uassert(8362701,
            "AnyCursor must have exactly one of 'firstBatch' or 'nextBatch'",
            hasFirst != hasNext);
}

}  // namespace mongo
