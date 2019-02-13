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

#pragma once

#include "mongo/client/dbclient_cursor.h"

namespace mongo {
// DBClientMockCursor supports only a small subset of DBClientCursor operations.
// It supports only iteration, including use of DBClientCursorBatchIterator.  If a batchsize
// is given, iteration is broken up into multiple batches at batchSize boundaries.
class DBClientMockCursor : public DBClientCursor {
public:
    DBClientMockCursor(mongo::DBClientBase* client,
                       const BSONArray& mockCollection,
                       unsigned long batchSize = 0)
        : mongo::DBClientCursor(client, NamespaceString(), 0, 0, 0),
          _collectionArray(mockCollection),
          _iter(_collectionArray),
          _batchSize(batchSize) {
        if (_batchSize)
            setBatchSize(_batchSize);
        fillNextBatch();
    }

    virtual ~DBClientMockCursor() {}

    bool more() override {
        if (_batchSize && batch.pos == _batchSize) {
            fillNextBatch();
        }
        return batch.pos < batch.objs.size();
    }

private:
    void fillNextBatch() {
        int leftInBatch = _batchSize;
        batch.objs.clear();
        while (_iter.more() && (!_batchSize || leftInBatch--)) {
            batch.objs.emplace_back(_iter.next().Obj());
        }
        batch.pos = 0;
    }
    // The BSONObjIterator expects the underlying BSONObj to stay in scope while the
    // iterator is in use, so we store it here.
    BSONArray _collectionArray;
    BSONObjIterator _iter;
    unsigned long _batchSize;

    // non-copyable , non-assignable
    DBClientMockCursor(const DBClientMockCursor&);
    DBClientMockCursor& operator=(const DBClientMockCursor&);
};

}  // namespace mongo
