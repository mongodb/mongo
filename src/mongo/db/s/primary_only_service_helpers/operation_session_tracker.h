/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/session/logical_session_id_gen.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Interface for persisting and retrieving operation session state. The OperationSessionTracker
 * delegates to this interface for all storage of session information, allowing different backends
 * (e.g., the DDL coordinator persists session info in its coordinator state document).
 */
class MONGO_MOD_OPEN OperationSessionPersistence {
public:
    virtual ~OperationSessionPersistence() = default;

    /**
     * Returns the currently persisted session, or boost::none if no session has been stored yet.
     */
    virtual boost::optional<OperationSessionInfo> readSession(OperationContext* opCtx) const = 0;

    /**
     * Persists the given session information, or clears it if 'osi' is boost::none. The
     * OperationSessionTracker will call this with boost::none when releasing a session.
     */
    virtual void writeSession(OperationContext* opCtx,
                              const boost::optional<OperationSessionInfo>& osi) = 0;
};

/**
 * Interface for establishing causality across a set of participants. Implementations
 * perform a no-op retryable write on their participant shards using the provided session
 * information, ensuring that subsequent reads on those shards reflect all prior writes made with
 * earlier txnNumbers on the same session. Additionally, this causes rogue primaries in a split
 * brain scenario to have their operations rejected, as they will be performed with a lower
 * txnNumber.
 */
class MONGO_MOD_OPEN CausalityBarrier {
public:
    virtual ~CausalityBarrier() = default;

    /**
     * Performs a no-op retryable write on the relevant participants using the given session info.
     */
    virtual void perform(OperationContext* opCtx, const OperationSessionInfo& osi) = 0;
};

/**
 * Manages the lifecycle of operation sessions used for retryable writes in primary-only service
 * operations. Sessions are lazily acquired from the InternalSessionPool on the first call to
 * getNextSession(), and the transaction number is advanced on each subsequent call. Session state
 * is persisted and retrieved via the provided OperationSessionPersistence, allowing the tracker
 * to resume from the persisted session after failover.
 */
class MONGO_MOD_PUBLIC OperationSessionTracker {
public:
    OperationSessionTracker(OperationSessionPersistence* persistence);

    /**
     * Advances and persists the txnNumber of the current session, then returns the
     * updated operation session information (OSI). If no session has been acquired yet, one is
     * acquired from the InternalSessionPool.
     */
    OperationSessionInfo getNextSession(OperationContext* opCtx);

    /**
     * Returns the currently persisted session, or boost::none if no session has been acquired yet.
     */
    boost::optional<OperationSessionInfo> getCurrentSession(OperationContext* opCtx) const;

    /**
     * Releases the current session back to the InternalSessionPool and clears the persisted
     * session state. No-op if no session is currently held.
     */
    void releaseSession(OperationContext* opCtx);

    /**
     * Advances the session via getNextSession() and then performs the given causality barrier,
     * ensuring that subsequent reads on the barrier's participants will reflect all prior writes.
     * At minimum, this should be called at the beginning of each new term when a PrimaryOnlyService
     * steps up.
     */
    void performCausalityBarrier(OperationContext* opCtx, CausalityBarrier& barrier);

private:
    OperationSessionPersistence* _persistence;
};

}  // namespace mongo
