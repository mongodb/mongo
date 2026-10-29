// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/session/session_txn_record_gen.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * A view over a transaction table record that validates all the necessary fields are present upon
 * construction and presents a non-optional interface for those fields.
 */
class [[MONGO_MOD_PUBLIC]] SessionTxnRecordForPrepareRecovery : private SessionTxnRecord {
public:
    using SessionTxnRecord::getLastWriteOpTime;
    using SessionTxnRecord::getSessionId;
    using SessionTxnRecord::getSizeMetadata;
    using SessionTxnRecord::getTxnNum;
    using SessionTxnRecord::getTxnRetryCounter;

    explicit SessionTxnRecordForPrepareRecovery(SessionTxnRecord&& txnRecord);

    // Some fields in a transaction record can be large, so only allow the move constructors.
    SessionTxnRecordForPrepareRecovery(const SessionTxnRecordForPrepareRecovery&) = delete;
    SessionTxnRecordForPrepareRecovery& operator=(const SessionTxnRecordForPrepareRecovery&) =
        delete;
    SessionTxnRecordForPrepareRecovery(SessionTxnRecordForPrepareRecovery&&) noexcept = default;
    SessionTxnRecordForPrepareRecovery& operator=(SessionTxnRecordForPrepareRecovery&&) noexcept =
        default;

    // Methods from the base class overwritten to return non-optional types.
    Timestamp getPrepareTimestamp() const;
    const std::vector<NamespaceString>& getAffectedNamespaces() const;
};

}  // namespace mongo
