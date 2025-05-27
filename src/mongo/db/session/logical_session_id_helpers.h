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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <initializer_list>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

constexpr size_t kMaximumUserNameLengthForLogicalSessions = 10000;

/**
 * Get the currently logged in user's UID digest.
 */
SHA256Block getLogicalSessionUserDigestForLoggedInUser(const OperationContext* opCtx);

/**
 * Get a user digest for a specific user/db identifier.
 */
SHA256Block getLogicalSessionUserDigestFor(StringData user, StringData db);

/**
 * Returns if the given session is a parent session, ie only has fields that could have come from an
 * external client.
 */
bool isParentSessionId(const LogicalSessionId& sessionId);

/**
 * Returns if the given session is a child session, ie it was created on behalf of an operation that
 * already had a session.
 */
bool isChildSession(const LogicalSessionId& sessionId);

/**
 * Returns the parent session id for the given session id if there is one.
 */
boost::optional<LogicalSessionId> getParentSessionId(const LogicalSessionId& sessionId);

/**
 * Returns the upconverted parent session id for the given session id if there is one.
 * Otherwise, returns the session id itself.
 */
LogicalSessionId castToParentSessionId(const LogicalSessionId& sessionId);

/**
 * Returns true if the session with the given session id is an internal session for internal
 * transactions for retryable writes.
 */
bool isInternalSessionForRetryableWrite(const LogicalSessionId& sessionId);

/**
 * Returns true if the txnRetryCounter is still the default value, meaning that for the relevant
 * internal transaction, no retries have occurred.
 */
bool isDefaultTxnRetryCounter(TxnRetryCounter txnRetryCounter);

/**
 * Returns true if the session with the given session id is an internal session for internal
 * transactions for non-retryable writes (i.e. writes in a session without a transaction number).
 */
bool isInternalSessionForNonRetryableWrite(const LogicalSessionId& sessionId);

/**
 * Helpers to make internal sessions.
 */
LogicalSessionId makeLogicalSessionIdWithTxnNumberAndUUID(const LogicalSessionId& parentLsid,
                                                          TxnNumber txnNumber);
LogicalSessionId makeLogicalSessionIdWithTxnUUID(const LogicalSessionId& parentLsid);

/**
 * Factory functions to generate logical session records.
 */
LogicalSessionId makeLogicalSessionId(const LogicalSessionFromClient& lsid,
                                      OperationContext* opCtx,
                                      std::initializer_list<Privilege> allowSpoof = {});
LogicalSessionId makeLogicalSessionId(OperationContext* opCtx);

/**
 * We recommend acquiring a system session through the session pool. It can be acquired through this
 * method InternalSessionPool::acquireSystemSession().
 */
LogicalSessionId makeSystemLogicalSessionId();

/**
 * Factory functions to make logical session records. The overloads that
 * take an OperationContext should be used when possible, as they will also set the
 * user information on the record.
 */
LogicalSessionRecord makeLogicalSessionRecord(const LogicalSessionId& lsid, Date_t lastUse);
LogicalSessionRecord makeLogicalSessionRecord(OperationContext* opCtx, Date_t lastUse);
LogicalSessionRecord makeLogicalSessionRecord(OperationContext* opCtx,
                                              const LogicalSessionId& lsid,
                                              Date_t lastUse);

LogicalSessionToClient makeLogicalSessionToClient(const LogicalSessionId& lsid);
LogicalSessionIdSet makeLogicalSessionIds(const std::vector<LogicalSessionFromClient>& sessions,
                                          OperationContext* opCtx,
                                          std::initializer_list<Privilege> allowSpoof = {});

namespace logical_session_id_helpers {

void serializeLsidAndTxnNumber(OperationContext* opCtx, BSONObjBuilder* builder);

void serializeLsid(OperationContext* opCtx, BSONObjBuilder* builder);

}  // namespace logical_session_id_helpers
}  // namespace mongo
