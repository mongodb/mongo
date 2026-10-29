// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/storage/wiredtiger/wiredtiger_prepared_transactions_iterator.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"

namespace mongo {
WiredTigerPreparedTransactionsIterator::WiredTigerPreparedTransactionsIterator(
    WiredTigerManagedSession managedSession)
    : _managedSession(std::move(managedSession)), _cursor(*_managedSession) {};

boost::optional<uint64_t> WiredTigerPreparedTransactionsIterator::next() {
    WT_CURSOR* cursor = _cursor.get();

    // Check if there are any more prepared transactions.
    int ret = cursor->next(cursor);
    auto status = wtRCToStatus(ret, *_managedSession);
    if (ret == WT_NOTFOUND) {
        return boost::none;
    }
    uassertStatusOK(status);

    // Get the prepared transaction id.
    uint64_t preparedId = 0;
    ret = cursor->get_key(cursor, &preparedId);
    status = wtRCToStatus(ret, *_managedSession);
    uassertStatusOK(status);
    return preparedId;
};

}  // namespace mongo
