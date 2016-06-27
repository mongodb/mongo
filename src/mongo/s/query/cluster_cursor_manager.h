/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/random.h"
#include "mongo/s/query/cluster_client_cursor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ClockSource;
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
 * with the kill*() suite of methods.  These simply mark the affected cursors as 'kill pending',
 * which can be cleaned up by later calls to the reapZombieCursors() method.
 *
 * No public methods throw exceptions, and all public methods are thread-safe.
 *
 * TODO: Add maxTimeMS support.  SERVER-19410.
 * TODO: Add method "size_t killCursorsOnNamespace(const NamespaceString& nss)" for
 *       dropCollection()?
 */
class ClusterCursorManager {
    MONGO_DISALLOW_COPYING(ClusterCursorManager);

public:
    //
    // Enum/struct declarations, for use with public methods below.
    //

    enum class CursorType {
        // Represents a cursor operating on an unsharded namespace.
        NamespaceNotSharded,

        // Represents a cursor operating on a sharded namespace.
        NamespaceSharded,
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
        // Count of open cursors registered with CursorType::NamespaceSharded.
        size_t cursorsSharded = 0;

        // Count of open cursors registered with CursorType::NamespaceNotSharded.
        size_t cursorsNotSharded = 0;

        // Count of pinned cursors.
        size_t cursorsPinned = 0;
    };

    /**
     * PinnedCursor is a moveable, non-copyable class representing ownership of a cursor that has
     * been leased from a ClusterCursorManager.
     *
     * A PinnedCursor can either be in a state where it owns a cursor, or can be in a null state
     * where it owns no cursor.  If a cursor is owned, the underlying cursor can be iterated with
     * next(), and the underlying cursor can be returned to the manager with the returnCursor()
     * method (and after it is returned, no cursor will be owned).
     *
     * Invoking the PinnedCursor's destructor while it owns a cursor will kill and return the
     * cursor.
     */
    class PinnedCursor {
        MONGO_DISALLOW_COPYING(PinnedCursor);

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
         * Calls next() on the underlying cursor.  Cannot be called after returnCursor() is called.
         * A cursor must be owned.
         *
         * Can block.
         */
        StatusWith<boost::optional<BSONObj>> next();

        /**
         * Returns whether or not the underlying cursor is tailing a capped collection.  Cannot be
         * called after returnCursor() is called.  A cursor must be owned.
         */
        bool isTailable() const;

        /**
         * Transfers ownership of the underlying cursor back to the manager.  A cursor must be
         * owned, and a cursor will no longer be owned after this method completes.
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
         * Returns the number of result documents returned so far by this cursor via the next()
         * method.
         */
        long long getNumReturnedSoFar() const;

        /**
         * Stashes 'obj' to be returned later by this cursor. A cursor must be owned.
         */
        void queueResult(const BSONObj& obj);

        /**
         * Returns whether or not all the remote cursors underlying this cursor have been
         * exhausted. Cannot be called after returnCursor() is called. A cursor must be owned.
         */
        bool remotesExhausted();

        /**
         * Sets the maxTimeMS value that the cursor should forward with any internally issued
         * getMore requests. A cursor must be owned.
         *
         * Returns a non-OK status if this cursor type does not support maxTimeMS on getMore (i.e.
         * if the cursor is not tailable + awaitData).
         */
        Status setAwaitDataTimeout(Milliseconds awaitDataTimeout);

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
                     std::unique_ptr<ClusterClientCursor> cursor,
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
    void shutdown();

    /**
     * Registers the given cursor with this manager, and returns the registered cursor's id, or
     * a non-OK status if something went wrong.
     *
     * 'cursor' must be non-null.  'cursorType' should reflect whether or not the cursor is
     * operating on a sharded namespace (this will be used for reporting purposes).
     * 'cursorLifetime' should reflect whether or not this cursor should be immune from the idle
     * cursor destruction procedure.
     *
     * On an error return, kills 'cursor'.
     *
     * Does not block.
     */
    StatusWith<CursorId> registerCursor(std::unique_ptr<ClusterClientCursor> cursor,
                                        const NamespaceString& nss,
                                        CursorType cursorType,
                                        CursorLifetime cursorLifetime);

    /**
     * Moves the given cursor to the 'pinned' state, and transfers ownership of the cursor to the
     * PinnedCursor object returned.  Cursors that are pinned must later be returned with
     * PinnedCursor::returnCursor().
     *
     * Only one client may pin a given cursor at a time.  If the given cursor is already pinned,
     * returns an error Status with code CursorInUse.  If the given cursor is not registered or has
     * a pending kill, returns an error Status with code CursorNotFound.
     *
     * This method updates the 'last active' time associated with the cursor to the current time.
     *
     * Does not block.
     */
    StatusWith<PinnedCursor> checkOutCursor(const NamespaceString& nss, CursorId cursorId);

    /**
     * Informs the manager that the given cursor should be killed.  The cursor need not necessarily
     * be in the 'idle' state, and the lifetime type of the cursor is ignored.
     *
     * If the given cursor is not registered, returns an error Status with code CursorNotFound.
     * Otherwise, marks the cursor as 'kill pending' and returns Status::OK().
     *
     * Does not block.
     */
    Status killCursor(const NamespaceString& nss, CursorId cursorId);

    /**
     * Informs the manager that all mortal cursors with a 'last active' time equal to or earlier
     * than 'cutoff' should be killed.  The cursors need not necessarily be in the 'idle' state.
     *
     * Does not block.
     */
    void killMortalCursorsInactiveSince(Date_t cutoff);

    /**
     * Informs the manager that all currently-registered cursors should be killed (regardless of
     * pinned status or lifetime type).
     *
     * Does not block.
     */
    void killAllCursors();

    /**
     * Attempts to performs a blocking kill and deletion of all non-pinned cursors that are marked
     * as 'kill pending'. Returns the number of cursors that were marked as inactive.
     *
     * If no other non-const methods are called simultaneously, it is guaranteed that this method
     * will delete all non-pinned cursors marked as 'kill pending'.  Otherwise, no such guarantee is
     * made (this is due to the fact that the blocking kill for each cursor is performed outside of
     * the cursor manager lock).
     *
     * Can block.
     */
    std::size_t reapZombieCursors();

    /**
     * Returns the number of open cursors on a ClusterCursorManager, broken down by type.
     *
     * Does not block.
     */
    Stats stats() const;

    /**
     * Returns the namespace associated with the given cursor id, by examining the 'namespace
     * prefix' portion of the cursor id.  A cursor with the given cursor id need not actually exist.
     * If no such namespace is associated with the 'namespace prefix' portion of the cursor id,
     * returns boost::none.
     *
     * This method is deprecated.  Use only when a cursor needs to be operated on in cases where a
     * namespace is not available (e.g. OP_KILL_CURSORS).
     *
     * Does not block.
     */
    boost::optional<NamespaceString> getNamespaceForCursorId(CursorId cursorId) const;

    void incrementCursorsTimedOut(size_t inc) {
        _cursorsTimedOut += inc;
    }

    size_t cursorsTimedOut() const {
        return _cursorsTimedOut;
    }

private:
    class CursorEntry;
    using CursorEntryMap = std::unordered_map<CursorId, CursorEntry>;

    /**
     * Transfers ownership of the given pinned cursor back to the manager, and moves the cursor to
     * the 'idle' state.
     *
     * If 'cursorState' is 'Exhausted', the cursor will be destroyed.  However, destruction will be
     * delayed until reapZombieCursors() is called under the following circumstances:
     *   - The cursor is already marked as 'kill pending'.
     *   - The cursor is managing open remote cursors which still need to be cleaned up.
     *
     * Thread-safe.
     *
     * Intentionally private.  Clients should use public methods on PinnedCursor to check a cursor
     * back in.
     */
    void checkInCursor(std::unique_ptr<ClusterClientCursor> cursor,
                       const NamespaceString& nss,
                       CursorId cursorId,
                       CursorState cursorState);

    /**
     * Returns a pointer to the CursorEntry for the given cursor.  If the given cursor is not
     * registered, returns null.
     *
     * Not thread-safe.
     */
    CursorEntry* getEntry_inlock(const NamespaceString& nss, CursorId cursorId);

    /**
     * De-registers the given cursor, and returns an owned pointer to the underlying
     * ClusterClientCursor object.
     *
     * If the given cursor is pinned, returns an error Status with code CursorInUse.  If the given
     * cursor is not registered, returns an error Status with code CursorNotFound.
     *
     * Not thread-safe.
     */
    StatusWith<std::unique_ptr<ClusterClientCursor>> detachCursor_inlock(const NamespaceString& nss,
                                                                         CursorId cursorId);

    /**
     * CursorEntry is a moveable, non-copyable container for a single cursor.
     */
    class CursorEntry {
        MONGO_DISALLOW_COPYING(CursorEntry);

    public:
        CursorEntry() = default;

        CursorEntry(std::unique_ptr<ClusterClientCursor> cursor,
                    CursorType cursorType,
                    CursorLifetime cursorLifetime,
                    Date_t lastActive)
            : _cursor(std::move(cursor)),
              _cursorType(cursorType),
              _cursorLifetime(cursorLifetime),
              _lastActive(lastActive) {
            invariant(_cursor);
        }

        CursorEntry(CursorEntry&& other) = default;
        CursorEntry& operator=(CursorEntry&& other) = default;

        bool getKillPending() const {
            return _killPending;
        }

        bool isInactive() const {
            return _isInactive;
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

        bool isCursorOwned() const {
            return static_cast<bool>(_cursor);
        }

        /**
         * Releases the cursor from this entry.  If the cursor has already been released, returns
         * null.
         */
        std::unique_ptr<ClusterClientCursor> releaseCursor() {
            return std::move(_cursor);
        }

        /**
         * Transfers ownership of the given released cursor back to this entry.
         */
        void returnCursor(std::unique_ptr<ClusterClientCursor> cursor) {
            invariant(cursor);
            invariant(!_cursor);
            _cursor = std::move(cursor);
        }

        void setKillPending() {
            _killPending = true;
        }

        void setInactive() {
            _isInactive = true;
        }

        void setLastActive(Date_t lastActive) {
            _lastActive = lastActive;
        }

    private:
        std::unique_ptr<ClusterClientCursor> _cursor;
        bool _killPending = false;
        bool _isInactive = false;
        CursorType _cursorType = CursorType::NamespaceNotSharded;
        CursorLifetime _cursorLifetime = CursorLifetime::Mortal;
        Date_t _lastActive;
    };

    /**
     * CursorEntryContainer is a moveable, non-copyable container for a set of cursors, where all
     * contained cursors share the same 32-bit prefix of their cursor id.
     */
    struct CursorEntryContainer {
        MONGO_DISALLOW_COPYING(CursorEntryContainer);

        CursorEntryContainer(uint32_t containerPrefix) : containerPrefix(containerPrefix) {}

        CursorEntryContainer(CursorEntryContainer&& other) = default;
        CursorEntryContainer& operator=(CursorEntryContainer&& other) = default;

        // Common cursor id prefix for all cursors in this container.
        uint32_t containerPrefix;

        // Map from cursor id to cursor entry.
        CursorEntryMap entryMap;
    };

    // Clock source.  Used when the 'last active' time for a cursor needs to be set/updated.  May be
    // concurrently accessed by multiple threads.
    ClockSource* _clockSource;

    // Synchronizes access to all private state variables below.
    mutable stdx::mutex _mutex;

    bool _inShutdown{false};

    // Randomness source.  Used for cursor id generation.
    PseudoRandom _pseudoRandom;

    // Map from cursor id prefix to associated namespace.  Exists only to provide namespace lookup
    // for (deprecated) getNamespaceForCursorId() method.
    //
    // A CursorId is a 64-bit type, made up of a 32-bit prefix and a 32-bit suffix.  When the first
    // cursor on a given namespace is registered, it is given a CursorId with a prefix that is
    // unique to that namespace, and an arbitrary suffix.  Cursors subsequently registered on that
    // namespace will all share the same prefix.
    //
    // Entries are added when the first cursor on the given namespace is registered, and removed
    // when the last cursor on the given namespace is destroyed.
    std::unordered_map<uint32_t, NamespaceString> _cursorIdPrefixToNamespaceMap;

    // Map from namespace to the CursorEntryContainer for that namespace.
    //
    // Entries are added when the first cursor on the given namespace is registered, and removed
    // when the last cursor on the given namespace is destroyed.
    std::unordered_map<NamespaceString, CursorEntryContainer, NamespaceString::Hasher>
        _namespaceToContainerMap;

    size_t _cursorsTimedOut = 0;
};

}  // namespace
