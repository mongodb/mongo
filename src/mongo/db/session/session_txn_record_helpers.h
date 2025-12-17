/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/session/session_txn_record_gen.h"

namespace MONGO_MOD_PUB mongo {

/**
 * A view over a transaction table record that validates all the necessary fields are present upon
 * construction and presents a non-optional interface for those fields.
 */
class MONGO_MOD_PUB SessionTxnRecordForPrepareRecovery : private SessionTxnRecord {
public:
    using SessionTxnRecord::getLastWriteOpTime;
    using SessionTxnRecord::getSessionId;
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

}  // namespace MONGO_MOD_PUB mongo
