// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <initializer_list>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

constexpr size_t kMaximumUserNameLengthForLogicalSessions = 10000;

/**
 * Get the currently logged in user's UID digest.
 */
SHA256Block getLogicalSessionUserDigestForLoggedInUser(const OperationContext* opCtx);

/**
 * Get a user digest for a specific user/db identifier.
 */
SHA256Block getLogicalSessionUserDigestFor(std::string_view user, std::string_view db);

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
[[MONGO_MOD_PRIVATE]] LogicalSessionId makeLogicalSessionIdWithTxnNumberAndUUID(
    const LogicalSessionId& parentLsid, TxnNumber txnNumber);
[[MONGO_MOD_PRIVATE]] LogicalSessionId makeLogicalSessionIdWithTxnUUID(
    const LogicalSessionId& parentLsid);

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
[[MONGO_MOD_USE_REPLACEMENT(InternalSessionPool::acquireSystemSession())]]
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

LogicalSessionId makeLogicalSessionIdForTest();

LogicalSessionId makeLogicalSessionIdWithTxnNumberAndUUIDForTest(
    boost::optional<LogicalSessionId> parentLsid = boost::none,
    boost::optional<TxnNumber> parentTxnNumber = boost::none);

LogicalSessionId makeLogicalSessionIdWithTxnUUIDForTest(
    boost::optional<LogicalSessionId> parentLsid = boost::none);

LogicalSessionRecord makeLogicalSessionRecordForTest();

namespace [[MONGO_MOD_PUBLIC]] logical_session_id_helpers {

void serializeLsidAndTxnNumber(OperationContext* opCtx, BSONObjBuilder* builder);

void serializeLsid(OperationContext* opCtx, BSONObjBuilder* builder);

}  // namespace logical_session_id_helpers
}  // namespace mongo
