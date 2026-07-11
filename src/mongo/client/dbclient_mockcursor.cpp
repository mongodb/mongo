// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/dbclient_mockcursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

MONGO_FAIL_POINT_DEFINE(mockCursorThrowErrorOnGetMore);

DBClientMockCursor::DBClientMockCursor(mongo::DBClientBase* client,
                                       const BSONArray& mockCollection,
                                       const bool provideResumeToken,
                                       unsigned long batchSize)
    : mongo::DBClientCursor(client, NamespaceString::kEmpty, 0 /*cursorId*/, false /*isExhaust*/),
      _collectionArray(mockCollection),
      _iter(_collectionArray),
      _provideResumeToken(provideResumeToken),
      _batchSize(batchSize) {
    if (_batchSize)
        setBatchSize(_batchSize);
    _fillNextBatch();
}

bool DBClientMockCursor::more() {
    if (_batchSize && _batch.pos == _batchSize) {
        _fillNextBatch();
    }
    return _batch.pos < _batch.objs.size();
}

void DBClientMockCursor::_fillNextBatch() {
    // Throw if requested via failpoint.
    mockCursorThrowErrorOnGetMore.execute([&](const BSONObj& data) {
        auto errorString = data["errorType"].valueStringDataSafe();
        auto errorCode = ErrorCodes::fromString(errorString);

        std::string message = str::stream()
            << "mockCursorThrowErrorOnGetMore throwing error for test: " << errorString;
        uasserted(errorCode, message);
    });

    int leftInBatch = _batchSize;
    _batch.objs.clear();
    while (_iter.more() && (!_batchSize || leftInBatch--)) {
        _batch.objs.emplace_back(_iter.next().Obj().getOwned());
    }
    _batch.pos = 0;

    // Store a mock resume token, if applicable.
    if (!_batch.objs.empty()) {
        auto lastId = _batch.objs.back()["_id"].numberInt();
        _postBatchResumeToken = BSON("n" << lastId);
    }
}

boost::optional<BSONObj> DBClientMockCursor::getPostBatchResumeToken() const {
    if (!_provideResumeToken) {
        return boost::none;
    }
    return _postBatchResumeToken;
}

}  // namespace mongo
