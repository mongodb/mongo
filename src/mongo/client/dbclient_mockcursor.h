// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/util/modules.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] DBClientMockCursor : public DBClientCursor {
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
