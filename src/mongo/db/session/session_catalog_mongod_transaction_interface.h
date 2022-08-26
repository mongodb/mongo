/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/session/session_catalog.h"              // for ScanSessionsCallbackFn
#include "mongo/db/session/session_txn_record_gen.h"       // for SessionTxnRecord
#include "mongo/db/transaction/transaction_participant.h"  // for SessionToKill

namespace mongo {

/**
 * This interface provides methods for the MongoDSessionCatalog implementation to access
 * multi-document transaction features, specifically functionality provided by the
 * TransactionParticipant class in the db/transaction library.
 */
class MongoDSessionCatalogTransactionInterface {
public:
    using ScanSessionsCallbackFn = SessionCatalog::ScanSessionsCallbackFn;

    virtual ~MongoDSessionCatalogTransactionInterface() = default;

    /**
     * Aborts the transaction, releasing transaction resources.
     */
    virtual void abortTransaction(OperationContext* opCtx, const SessionTxnRecord& txnRecord) = 0;

    /**
     * Marks the session as requiring refresh. Used when the session state has been modified
     * externally, such as through a direct write to the transactions table.
     */
    virtual void invalidateSessionToKill(OperationContext* opCtx, const SessionToKill& session) = 0;

    /**
     * Returns a 'parentSessionWorkerFn' that can be passed to
     * SessionCatalog::scanSessionsForReap().
     *
     * Accepts an output parameter for the parent session's TxnNumber.
     */
    virtual ScanSessionsCallbackFn makeParentSessionWorkerFnForReap(
        TxnNumber* parentSessionActiveTxnNumber) = 0;

    /**
     * Returns a 'childSessionWorkerFn' that can be passed to SessionCatalog::scanSessionsForReap().
     *
     * Accepts a reference to the parent session's TxnNumber.
     */
    virtual ScanSessionsCallbackFn makeChildSessionWorkerFnForReap(
        const TxnNumber& parentSessionActiveTxnNumber) = 0;
};

}  // namespace mongo
