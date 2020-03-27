/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_mockcursor.h"

#include "mongo/client/dbclient_cursor.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(mockCursorThrowErrorOnGetMore);

DBClientMockCursor::DBClientMockCursor(mongo::DBClientBase* client,
                                       const BSONArray& mockCollection,
                                       const bool provideResumeToken,
                                       unsigned long batchSize)
    : mongo::DBClientCursor(client, NamespaceString(), 0, 0, 0),
      _collectionArray(mockCollection),
      _iter(_collectionArray),
      _provideResumeToken(provideResumeToken),
      _batchSize(batchSize) {
    if (_batchSize)
        setBatchSize(_batchSize);
    _fillNextBatch();
}

bool DBClientMockCursor::more() {

    // Throw if requested via failpoint.
    mockCursorThrowErrorOnGetMore.execute([&](const BSONObj& data) {
        auto errorString = data["errorType"].valueStringDataSafe();
        auto errorCode = ErrorCodes::fromString(errorString);

        std::string message = str::stream()
            << "mockCursorThrowErrorOnGetMore throwing error for test: " << errorString;
        uasserted(errorCode, message);
    });

    if (_batchSize && batch.pos == _batchSize) {
        _fillNextBatch();
    }
    return batch.pos < batch.objs.size();
}

void DBClientMockCursor::_fillNextBatch() {
    int leftInBatch = _batchSize;
    batch.objs.clear();
    while (_iter.more() && (!_batchSize || leftInBatch--)) {
        batch.objs.emplace_back(_iter.next().Obj().getOwned());
    }
    batch.pos = 0;

    // Store a mock resume token, if applicable.
    if (!batch.objs.empty()) {
        auto lastId = batch.objs.back()["_id"].numberInt();
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
