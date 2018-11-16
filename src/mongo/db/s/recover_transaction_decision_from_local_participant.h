
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

namespace mongo {

class OperationContext;

/**
 * Examines the local participant's decision for the transaction number on the OperationContext and
 * returns without throwing if the local participant's decision was commit. Otherwise, throws one of
 * the following errors:
 *
 * - If the local participant has a higher transaction number, throws TransactionTooOld.
 * - If the local participant is in prepare, throws an anonymous error code, because either the
 * request to recover the decision was a delayed message or a byzantine message.
 * - If the local participant has a lower transaction number, starts a transaction at the
 * transaction number on the OperationContext, aborts it, and throws NoSuchTransaction.
 * - If the local participant has the same transaction number and:
 *    -- the transaction number corresponds to a retryable write, throws NoSuchTransaction
 *    -- is already aborted, throws NoSuchTransaction
 *    -- is in progress, aborts the transaction and throws NoSuchTransaction
 *
 * Sets the Client last OpTime to the system last OpTime to ensure the caller waits for writeConcern
 * of the decision.
 */
void recoverDecisionFromLocalParticipantOrAbortLocalParticipant(OperationContext* opCtx);

}  // namespace mongo
