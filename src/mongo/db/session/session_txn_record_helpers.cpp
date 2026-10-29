// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/session_txn_record_helpers.h"

namespace mongo {

SessionTxnRecordForPrepareRecovery::SessionTxnRecordForPrepareRecovery(SessionTxnRecord&& txnRecord)
    : SessionTxnRecord(std::move(txnRecord)) {
    uassert(11372904,
            "Can't reclaim a prepared transaction without affected namespaces",
            SessionTxnRecord::getAffectedNamespaces());
    uassert(11372905,
            "Can't reclaim a prepared transaction without state",
            SessionTxnRecord::getState());
    uassert(11372906,
            "Can't reclaim a prepared transaction with state other than prepared",
            *SessionTxnRecord::getState() == DurableTxnStateEnum::kPrepared);
}

Timestamp SessionTxnRecordForPrepareRecovery::getPrepareTimestamp() const {
    return SessionTxnRecord::getLastWriteOpTime().getTimestamp();
}

const std::vector<NamespaceString>& SessionTxnRecordForPrepareRecovery::getAffectedNamespaces()
    const {
    return *SessionTxnRecord::getAffectedNamespaces();
}

static_assert(std::is_nothrow_move_constructible_v<SessionTxnRecordForPrepareRecovery>);

}  // namespace mongo
