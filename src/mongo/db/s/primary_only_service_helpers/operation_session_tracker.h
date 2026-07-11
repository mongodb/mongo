// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/session/logical_session_id_gen.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Interface for persisting and retrieving operation session state. The OperationSessionTracker
 * delegates to this interface for all storage of session information, allowing different backends
 * (e.g., the DDL coordinator persists session info in its coordinator state document).
 */
class [[MONGO_MOD_OPEN]] OperationSessionPersistence {
public:
    virtual ~OperationSessionPersistence() = default;

    /**
     * Returns the currently persisted session, or boost::none if no session has been stored yet.
     */
    virtual boost::optional<OperationSessionInfo> readSession(OperationContext* opCtx) const = 0;

    /**
     * Persists the given session information, or clears it if 'osi' is boost::none. The
     * OperationSessionTracker will call this with boost::none when releasing a session.
     *
     * Implementations must ensure the write is majority-committed before returning, so that
     * the session cannot be used before it is durably recorded. This can be achieved either by
     * writing with a majority write concern or by explicitly waiting for majority replication
     * after the write.
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
class [[MONGO_MOD_OPEN]] CausalityBarrier {
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
class [[MONGO_MOD_PUBLIC]] OperationSessionTracker {
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
