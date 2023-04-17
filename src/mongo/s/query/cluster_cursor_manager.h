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

#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/cursor_id.h"
#include "mongo/db/generic_cursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/s/query/cluster_client_cursor.h"
#include "mongo/s/query/cluster_client_cursor_guard.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ClockSource;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * ClusterCursorManager is a container for ClusterClientCursor objects.  It manages the lifetime of
 * its registered cursors and tracks basic information about them.
 *
 * Each registered cursor is either in a 'pinned' or an 'idle' state.  Registered cursors must be
 * pinned in order to iterate results, and cursors may only be pinned by one client at a time (this
 * ensures that the result stream is only directed to a single client at a time).  Pinning a cursor
 * transfers ownership of the cursor to a PinnedCursor object (although the manager maintains
 * information about registered cursors even when they're pinned).  Ownership is transferred back to
 * the manager by calling PinnedCursor::returnCursor().
 *
 * The manager supports killing of registered cursors, either through the PinnedCursor object or
 * with the kill*() suite of methods.
 *
 * No public methods throw exceptions, and all public methods are thread-safe.
 */
class ClusterCursorManager {
    ClusterCursorManager(const ClusterCursorManager&) = delete;
    ClusterCursorManager& operator=(const ClusterCursorManager&) = delete;

public:
    //
    // Enum/struct declarations, for use with public methods below.
    //

    enum class CursorType {
        // Represents a cursor retrieving data from a single remote source.
        SingleTarget,

        // Represents a cursor retrieving data from multiple remote sources.
        MultiTarget,

        // Represents a cursor retrieving data queued in memory on the router.
        QueuedData,
    };

    enum class CursorLifetime {
        // Represents a cursor that should be killed automatically after a period of inactivity.
        Mortal,

        // Represents a "no timeout" cursor.
        Immortal,
    };

    enum class CursorState {
        // Represents a non-exhausted cursor.
        NotExhausted,

        // Represents an exhausted cursor.
        Exhausted,
    };

    struct Stats {
        // Count of open cursors registered with CursorType::MultiTarget.
        size_t cursorsMultiTarget = 0;

        // Count of open cursors registered with CursorType::SingleTarget.
        size_t cursorsSingleTarget = 0;

        // Count of open cursors registered with CursorType::QueuedData.
        size_t cursorsQueuedData = 0;

        // Count of pinned cursors.
        size_t cursorsPinned = 0;
    };

    // Represents a function that may be passed into a ClusterCursorManager method which checks
    // whether the current client is authorized to perform the operation in question. The function
    // will be passed the list of users authorized to use the cursor.
    using AuthzCheckFn = std::function<Status(const boost::optional<UserName>&)>;

    /**
     * PinnedCursor is a moveable, non-copyable class representing ownership of a cursor that has
     * been leased from a ClusterCursorManager.
     *
     * A PinnedCursor can either be in a state where it owns a cursor, or can be in a null state
     * where it owns no cursor.  If a cursor is owned, the underlying cursor can be iterated with
     * next(), and the underlying cursor can be returned to the manager with the returnCursor()
     * method (and after it is returned, no cursor will be owned). When a PinnedCursor is created,
     * the underlying cursor is attached to the current OperationContext.
     *
     * Invoking the PinnedCursor's destructor while it owns a cursor will kill, detach from the
     * current OperationContext, and return the cursor.
     */
    class PinnedCursor {
        PinnedCursor(const PinnedCursor&) = delete;
        PinnedCursor& operator=(const PinnedCursor&) = delete;

    public:
        /**
         * Creates a PinnedCursor owning no cursor.
         */
        PinnedCursor() = default;

        /**
         * If a cursor is not owned, performs no action.  Otherwise, informs the manager that the
         * cursor should be killed, and transfers ownership of the cursor back to the manager.
         */
        ~PinnedCursor();

        /**
         * Creates a PinnedCursor by moving from 'other'.
         */
        PinnedCursor(PinnedCursor&& other);

        /**
         * Moves 'other' into '*this'.  If '*this' was owning a cursor, informs the manager that the
         * cursor should be killed, and transfers ownership of the cursor back to the manager.
         */
        PinnedCursor& operator=(PinnedCursor&& other);

        /**
         * Returns a pointer to the ClusterClientCursor that this PinnedCursor owns. A cursor must
         * be owned.
         */
        ClusterClientCursor* operator->() const {
            invariant(_cursor);
            return _cursor.get();
        }

        /**
         * Transfers ownership of the underlying cursor back to the manager, and detaches it from
         * the current OperationContext. A cursor must be owned, and a cursor will no longer be
         * owned after this method completes.
         *
         * If 'Exhausted' is passed, the manager will de-register and destroy the cursor after it
         * is returned.
         */
        void returnCursor(CursorState cursorState);

        /**
         * Returns the cursor id for the underlying cursor, or zero if no cursor is owned.
         */
        CursorId getCursorId() const;

        /**
         * Returns a GenericCursor version of the pinned cursor.
         */
        GenericCursor toGenericCursor() const;

    private:
        // ClusterCursorManager is a friend so that its methods can call the PinnedCursor
        // constructor declared below, which is private to prevent clients from calling it directly.
        friend class ClusterCursorManager;

        /**
         * Creates a PinnedCursor owning the given cursor, which must be checked out from the given
         * manager.  Does not take ownership of 'manager'.  'manager' and 'cursor' must be non-null,
         * and 'cursorId' must be non-zero.
         */
        PinnedCursor(ClusterCursorManager* manager,
                     ClusterClientCursorGuard&& cursor,
                     const NamespaceString& nss,
                     CursorId cursorId);

        /**
         * Informs the manager that the cursor should be killed, and transfers ownership of the
         * cursor back to the manager.  A cursor must be owned.
         */
        void returnAndKillCursor();

        ClusterCursorManager* _manager = nullptr;
        std::unique_ptr<ClusterClientCursor> _cursor;
        NamespaceString _nss;
        CursorId _cursorId = 0;
    };

    /**
     * CursorEntry is a movable, non-copyable container for a single cursor.
     */
    class CursorEntry {
    public:
        CursorEntry() = default;

        CursorEntry(std::unique_ptr<ClusterClientCursor> cursor,
                    CursorType cursorType,
                    CursorLifetime cursorLifetime,
                    Date_t lastActive,
                    boost::optional<UserName> authenticatedUser,
                    UUID clientUUID,
                    boost::optional<OperationKey> opKey,
                    NamespaceString nss)
            : _cursor(std::move(cursor)),
              _cursorType(cursorType),
              _cursorLifetime(cursorLifetime),
              _lastActive(lastActive),
              _lsid(_cursor->getLsid()),
              _opKey(std::move(opKey)),
              _nss(std::move(nss)),
              _originatingClient(std::move(clientUUID)),
              _authenticatedUser(std::move(authenticatedUser)) {
            invariant(_cursor);
        }

        CursorEntry(const CursorEntry&) = delete;
        CursorEntry& operator=(const CursorEntry&) = delete;

        CursorEntry(CursorEntry&& other) = default;
        CursorEntry& operator=(CursorEntry&& other) = default;

        bool isKillPending() const {
            // A cursor is kill pending if it's checked out by an OperationContext that was
            // interrupted.
            if (!_operationUsingCursor) {
                return false;
            }

            // Must hold the Client lock when calling isKillPending().
            stdx::unique_lock<Client> lk(*_operationUsingCursor->getClient());
            return _operationUsingCursor->isKillPending();
        }

        CursorType getCursorType() const {
            return _cursorType;
        }

        CursorLifetime getLifetimeType() const {
            return _cursorLifetime;
        }

        Date_t getLastActive() const {
            return _lastActive;
        }

        boost::optional<LogicalSessionId> getLsid() const {
            return _lsid;
        }

        boost::optional<OperationKey> getOperationKey() const {
            return _opKey;
        }

        const NamespaceString& getNamespace() const {
            return _nss;
        }

        /**
         * Returns a cursor guard holding the cursor owned by this CursorEntry for an operation to
         * use. Only one operation may use the cursor at a time, so callers should check that
         * getOperationUsingCursor() returns null before using this function. Callers may not pass
         * nullptr for opCtx. Ownership of the cursor is given to the returned
         * ClusterClientCursorGuard; callers that want to assume ownership over the cursor directly
         * must unpack the cursor from the returned guard.
         */
        ClusterClientCursorGuard releaseCursor(OperationContext* opCtx) {
            invariant(!_operationUsingCursor);
            invariant(_cursor);
            invariant(opCtx);
            _operationUsingCursor = opCtx;
            return ClusterClientCursorGuard(opCtx, std::move(_cursor));
        }

        /**
         * Creates a generic cursor from the cursor inside this entry. Should only be called on
         * idle cursors. The caller must supply the cursorId and namespace because the CursorEntry
         * does not have access to them.  Cannot be called if this CursorEntry does not own an
         * underlying ClusterClientCursor.
         */
        GenericCursor cursorToGenericCursor(CursorId cursorId, const NamespaceString& ns) const;

        OperationContext* getOperationUsingCursor() const {
            return _operationUsingCursor;
        }

        /**
         * Indicate that the cursor is no longer in use by an operation. Once this is called,
         * another operation may check the cursor out.
         */
        void returnCursor(std::unique_ptr<ClusterClientCursor> cursor) {
            invariant(cursor);
            invariant(!_cursor);
            invariant(_operationUsingCursor);

            _cursor = std::move(cursor);
            _operationUsingCursor = nullptr;
        }

        void setLastActive(Date_t lastActive) {
            _lastActive = lastActive;
        }

        const boost::optional<UserName>& getAuthenticatedUser() const {
            return _authenticatedUser;
        }

        const UUID& originatingClientUuid() const {
            return _originatingClient;
        }

    private:
        std::unique_ptr<ClusterClientCursor> _cursor;
        CursorType _cursorType = CursorType::SingleTarget;
        CursorLifetime _cursorLifetime = CursorLifetime::Mortal;
        Date_t _lastActive;
        boost::optional<LogicalSessionId> _lsid;

        /**
         * The client OperationKey from the OperationContext at the time of registering a cursor.
         */
        boost::optional<OperationKey> _opKey;

        NamespaceString _nss;

        /**
         * Current operation using the cursor. Non-null if the cursor is checked out.
         */
        OperationContext* _operationUsingCursor = nullptr;

        /**
         * The UUID of the Client that opened the cursor.
         */
        UUID _originatingClient;

        /**
         * The set of users authorized to use this cursor.
         */
        boost::optional<UserName> _authenticatedUser;
    };

    /**
     * Constructs an empty manager.
     *
     * Does not take ownership of 'clockSource'.  'clockSource' must refer to a non-null clock
     * source that is valid for the lifetime of the constructed ClusterCursorManager.
     */
    explicit ClusterCursorManager(ClockSource* clockSource);

    /**
     * Can only be called if the manager no longer owns any cursors.
     */
    ~ClusterCursorManager();

    /**
     * Kills and reaps all cursors currently owned by this cursor manager, and puts the manager
     * into the shutting down state where it will not accept any new cursors for registration.
     */
    void shutdown(OperationContext* opCtx);

    /**
     * Registers the given cursor with this manager, and returns the registered cursor's id, or
     * a non-OK status if something went wrong.
     *
     * 'cursor' must be non-null.  'cursorType' should reflect whether or not the cursor is
     * operating on a sharded namespace (this will be used for reporting purposes).
     * 'cursorLifetime' should reflect whether or not this cursor should be immune from the idle
     * cursor destruction procedure.
     *
     * If the OperationContext has a deadline set (from a maxTimeMS), stashes the remaining time
     * limit on 'cursor' for use in subsequent getMores.
     *
     * On an error return, kills 'cursor'.
     *
     * Does not block.
     */
    StatusWith<CursorId> registerCursor(OperationContext* opCtx,
                                        std::unique_ptr<ClusterClientCursor> cursor,
                                        const NamespaceString& nss,
                                        CursorType cursorType,
                                        CursorLifetime cursorLifetime,
                                        const boost::optional<UserName>& authenticatedUser);

    /**
     * Moves the given cursor to the 'pinned' state, and transfers ownership of the cursor to the
     * PinnedCursor object returned.  Cursors that are pinned must later be returned with
     * PinnedCursor::returnCursor().
     *
     * Only one client may pin a given cursor at a time.  If the given cursor is already pinned,
     * returns an error Status with code CursorInUse.  If the given cursor is not registered or has
     * a pending kill, returns an error Status with code CursorNotFound.
     *
     * Checking out a cursor will attach it to the given operation context.
     *
     * 'authChecker' is function that will be called with the list of users authorized to use this
     * cursor. This function should check whether the current client is also authorized to use this
     * cursor, and if not, return an error status, which will cause checkOutCursor to fail.
     *
     * If 'checkSessionAuth' is 'kCheckSession' or left unspecified, this function also checks if
     * the current session in the specified 'opCtx' has privilege to access the cursor specified by
     * 'id.' In this case, this function returns a 'mongo::Status' with information regarding the
     * nature of the inaccessability when the cursor is not accessible. If 'kNoCheckSession' is
     * passed for 'checkSessionAuth,' this function does not check if the current session is
     * authorized to access the cursor with the given id.
     *
     * This method updates the 'last active' time associated with the cursor to the current time.
     *
     * Does not block.
     */
    enum AuthCheck { kCheckSession = true, kNoCheckSession = false };
    StatusWith<PinnedCursor> checkOutCursor(CursorId cursorId,
                                            OperationContext* opCtx,
                                            AuthzCheckFn authChecker,
                                            AuthCheck checkSessionAuth = kCheckSession);

    /**
     * This method will find the given cursor, and if it exists, call 'authChecker', passing the
     * list of users authorized to use the cursor. Will propagate the return value of authChecker.
     */
    Status checkAuthForKillCursors(OperationContext* opCtx,
                                   CursorId cursorId,
                                   AuthzCheckFn authChecker);


    /**
     * Informs the manager that the given cursor should be killed.  The cursor need not necessarily
     * be in the 'idle' state, and the lifetime type of the cursor is ignored.
     *
     * If the given cursor is not registered, returns an error Status with code CursorNotFound.
     * Otherwise, marks the cursor as 'kill pending' and returns Status::OK().
     *
     * A thread which is currently using a cursor may not call killCursor() on it, but rather
     * should kill the cursor by checking it back into the manager in the exhausted state.
     *
     * May block waiting for other threads to finish, but does not block on the network.
     */
    Status killCursor(OperationContext* opCtx, CursorId cursorId);

    /**
     * Kill the cursors satisfying the given predicate. Returns the number of cursors killed.
     */
    std::size_t killCursorsSatisfying(
        OperationContext* opCtx, const std::function<bool(CursorId, const CursorEntry&)>& pred);

    /**
     * Informs the manager that all mortal cursors with a 'last active' time equal to or earlier
     * than 'cutoff' should be killed.  The cursors need not necessarily be in the 'idle' state.
     * The number of killed cursors is added to '_cursorsTimedOut' counter.
     *
     * May block waiting for other threads to finish, but does not block on the network.
     *
     * Returns the number of cursors that were killed due to inactivity.
     */
    std::size_t killMortalCursorsInactiveSince(OperationContext* opCtx, Date_t cutoff);

    /**
     * Kills all cursors which are registered at the time of the call. If a cursor is registered
     * while this function is running, it may not be killed. If the caller wants to guarantee that
     * all cursors are killed, shutdown() should be used instead.
     *
     * May block waiting for other threads to finish, but does not block on the network.
     */
    void killAllCursors(OperationContext* opCtx);

    /**
     * Returns the number of open cursors on a ClusterCursorManager, broken down by type.
     *
     * Does not block.
     */
    Stats stats() const;

    /**
     * Appends sessions that have open cursors in this cursor manager to the given set of lsids.
     */
    void appendActiveSessions(LogicalSessionIdSet* lsids) const;

    /**
     * Returns a list of GenericCursors for all idle (non-pinned) cursors in the cursor manager.
     */
    std::vector<GenericCursor> getIdleCursors(
        const OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode) const;

    std::pair<Status, int> killCursorsWithMatchingSessions(OperationContext* opCtx,
                                                           const SessionKiller::Matcher& matcher);

    /**
     * Returns a list of all open cursors for the given session.
     */
    stdx::unordered_set<CursorId> getCursorsForSession(LogicalSessionId lsid) const;

    size_t cursorsTimedOut() const;

private:
    using CursorEntryMap = stdx::unordered_map<CursorId, CursorEntry>;

    /**
     * Transfers ownership of the given pinned cursor back to the manager, and moves the cursor to
     * the 'idle' state.
     *
     * If 'cursorState' is 'Exhausted', the cursor will be destroyed.
     *
     * Thread-safe.
     *
     * Intentionally private.  Clients should use public methods on PinnedCursor to check a cursor
     * back in.
     */
    void checkInCursor(std::unique_ptr<ClusterClientCursor> cursor,
                       CursorId cursorId,
                       CursorState cursorState);

    /**
     * Will detach a cursor, release the lock and then call kill() on it.
     */
    void detachAndKillCursor(stdx::unique_lock<Latch> lk,
                             OperationContext* opCtx,
                             CursorId cursorId);

    /**
     * Returns a pointer to the CursorEntry for the given cursor.  If the given cursor is not
     * registered, returns null.
     *
     * Not thread-safe.
     */
    CursorEntry* _getEntry(WithLock, CursorId cursorId);

    /**
     * Allocates a new cursor id (a positive 64 bit number) that is not already in use.
     */
    CursorId _allocateCursorId();

    /**
     * De-registers the given cursor, and returns an owned pointer to the underlying
     * ClusterClientCursor object.
     *
     * If the given cursor is pinned, returns an error Status with code CursorInUse.  If the given
     * cursor is not registered, returns an error Status with code CursorNotFound.
     *
     * Not thread-safe.
     */
    StatusWith<ClusterClientCursorGuard> _detachCursor(WithLock,
                                                       OperationContext* opCtx,
                                                       CursorId cursorId);

    /**
     * Flags the OperationContext that's using the given cursor as interrupted.
     */
    void killOperationUsingCursor(WithLock, CursorEntry* entry);

    // Clock source.  Used when the 'last active' time for a cursor needs to be set/updated.  May be
    // concurrently accessed by multiple threads.
    ClockSource* _clockSource;

    // Synchronizes access to all private state variables below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ClusterCursorManager::_mutex");

    bool _inShutdown{false};

    // Randomness source.  Used for cursor id generation.
    const int64_t _randomSeed;
    PseudoRandom _pseudoRandom;

    // Map from CursorId to CursorEntry.
    CursorEntryMap _cursorEntryMap;

    size_t _cursorsTimedOut = 0;
};

/**
 * Record metrics for the current operation on opDebug and aggregates those metrics for telemetry
 * use. If a cursor is provided (via ClusterClientCursorGuard or
 * ClusterCursorManager::PinnedCursor), metrics are aggregated on the cursor; otherwise, metrics are
 * written directly to the telemetry store.
 */
void collectTelemetryMongos(OperationContext* opCtx,
                            const BSONObj& originatingCommand,
                            long long nreturned);
void collectTelemetryMongos(OperationContext* opCtx,
                            ClusterClientCursorGuard& cursor,
                            long long nreturned);
void collectTelemetryMongos(OperationContext* opCtx,
                            ClusterCursorManager::PinnedCursor& cursor,
                            long long nreturned);

}  // namespace mongo
