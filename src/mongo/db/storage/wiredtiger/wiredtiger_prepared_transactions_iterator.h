// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/storage/prepared_transactions_iterator.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"

#include <cstdint>

namespace mongo {
class WiredTigerPreparedTransactionsIterator : public PreparedTransactionsIterator {
public:
    WiredTigerPreparedTransactionsIterator(WiredTigerManagedSession session);

    // Returns the id of a prepared transaction that has been unclaimed on startup recovery or an
    // empty boost::optional if there are no more prepared transaction ids to return.
    boost::optional<uint64_t> next() override;

private:
    WiredTigerManagedSession _managedSession;
    WiredTigerPrepareCursor _cursor;
};

}  // namespace mongo
