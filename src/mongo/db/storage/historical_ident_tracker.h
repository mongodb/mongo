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

#include <list>
#include <string>
#include <unordered_map>

#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Keeps track of historical ident information when a collection is renamed or dropped.
 */
class HistoricalIdentTracker final {
public:
    HistoricalIdentTracker(const HistoricalIdentTracker&) = delete;
    HistoricalIdentTracker(HistoricalIdentTracker&&) = delete;

    static HistoricalIdentTracker& get(ServiceContext* svcCtx);
    static HistoricalIdentTracker& get(OperationContext* opCtx);

    HistoricalIdentTracker() = default;
    ~HistoricalIdentTracker() = default;

    /**
     * Returns the historical namespace and UUID for 'ident' at 'timestamp'. Returns boost::none if
     * there was no historical namespace.
     */
    boost::optional<std::pair<NamespaceString, UUID>> lookup(const std::string& ident,
                                                             Timestamp timestamp) const;

    /**
     * Pins the historical content to the given timestamp, preventing it from being removed.
     *
     * This is necessary for backup cursors, which need to report the namespace and UUID at the time
     * of the checkpoint the backup is being taken on. When a backup cursor is open, it pins the
     * checkpoint the backup is being taken on. Checkpoints can still be taken, which advances the
     * last checkpoint timestamp and would remove historical content needed by the open backup
     * cursor. This method prevents that from happening by pinning the content.
     *
     * The checkpoint timestamp of the backup can be earlier than the oldest timestamp, which
     * prevents us from opening a snapshot at the checkpoint timestamp as history before the oldest
     * timestamp is discarded.
     */
    void pinAtTimestamp(Timestamp timestamp);
    void unpin();

    /**
     * Records the idents namespace and UUID before it was renamed.
     */
    void recordRename(const std::string& ident,
                      const NamespaceString& oldNss,
                      const UUID& uuid,
                      Timestamp timestamp) {
        _addHistoricalIdent(ident, oldNss, uuid, timestamp);
    }

    /**
     * Records the idents namespace and UUID before it was dropped.
     */
    void recordDrop(const std::string& ident,
                    const NamespaceString& nss,
                    const UUID& uuid,
                    Timestamp timestamp) {
        _addHistoricalIdent(ident, nss, uuid, timestamp);
    }

    /**
     * Removes historical content that is no longer necessary. This is anything older than the last
     * checkpoint timestamp.
     *
     * If there's a pinned timestamp, min(timestamp, _pinnedTimestamp) is used.
     */
    void removeEntriesOlderThan(Timestamp timestamp);

    /**
     * Historical content added may not be stable yet and can be rolled back. When rollback to
     * stable runs, we need to remove any historical content that is considered current.
     *
     * If there's a pinned timestamp, max(timestamp, _pinnedTimestamp) is used.
     */
    void rollbackTo(Timestamp timestamp);

private:
    /**
     * Helper function for recordRename() and recordDrop().
     *
     * Appends a new historical entry with 'nss' and 'uuid' for 'ident' in '_historicalIdents'.
     * Sets the 'end' timestamp to be 'timestamp - 1'.
     * Sets the 'start' timestamp to the timestamp of the last entry + 1, or Timestamp::min() if
     * there was no earlier entry.
     */
    void _addHistoricalIdent(const std::string& ident,
                             const NamespaceString& nss,
                             const UUID& uuid,
                             Timestamp timestamp);

    struct HistoricalIdentEntry {
        const NamespaceString nss;
        const UUID uuid;
        Timestamp start;
        Timestamp end;
    };

    // Protects all the member variables below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("HistoricalIdentTracker::_mutex");
    stdx::unordered_map<std::string, std::list<HistoricalIdentEntry>> _historicalIdents;
    Timestamp _pinnedTimestamp;
};

}  // namespace mongo
