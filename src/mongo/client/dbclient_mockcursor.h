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

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_cursor.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class DBClientMockCursor : public DBClientCursor {
public:
    DBClientMockCursor(mongo::DBClientBase* client,
                       const BSONArray& mockCollection,
                       bool provideResumeToken = false,
                       unsigned long batchSize = 0);

    ~DBClientMockCursor() override {}

    bool more() override;

    // Override to return a mock resume token.
    // The format of the token is simply {n: <idOfLastDocReturned>}.
    boost::optional<BSONObj> getPostBatchResumeToken() const override;

private:
    void _fillNextBatch();

    // The BSONObjIterator expects the underlying BSONObj to stay in scope while the
    // iterator is in use, so we store it here.
    BSONArray _collectionArray;
    BSONObjIterator _iter;

    // A simple mock resume token that contains the id of the last document returned.
    boost::optional<BSONObj> _postBatchResumeToken;
    bool _provideResumeToken = false;

    unsigned long _batchSize;

    // non-copyable , non-assignable
    DBClientMockCursor(const DBClientMockCursor&);
    DBClientMockCursor& operator=(const DBClientMockCursor&);
};

}  // namespace mongo
