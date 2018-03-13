/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/s/database_version_gen.h"

namespace mongo {

class OperationContext;

/**
 * Synchronizes access to this shard server's cached database version for Database.
 */
class DatabaseShardingState {
    MONGO_DISALLOW_COPYING(DatabaseShardingState);

public:
    static const Database::Decoration<DatabaseShardingState> get;

    DatabaseShardingState();
    ~DatabaseShardingState() = default;

    /**
     * Methods to control the databases's critical section. Must be called with the database X lock
     * held.
     */
    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx);
    void enterCriticalSectionCommitPhase(OperationContext* opCtx);
    void exitCriticalSection(OperationContext* opCtx,
                             boost::optional<DatabaseVersion> newDbVersion);

    auto getCriticalSectionSignal(ShardingMigrationCriticalSection::Operation op) const {
        return _critSec.getSignal(op);
    }

    /**
     * Sets this shard server's cached dbVersion to newVersion.
     *
     * Invariants that the caller holds the DBLock in X mode.
     */
    void setDbVersion(OperationContext* opCtx, boost::optional<DatabaseVersion> newVersion);

    /**
     * If _critSecSignal is non-null, always throws StaleDbVersion.
     * Otherwise, if there is a client dbVersion on the OperationContext, compares it with this
     * shard server's cached dbVersion and throws StaleDbVersion if they do not match.
     */
    void checkDbVersion(OperationContext* opCtx) const;

private:
    // Modifying the state below requires holding the DBLock in X mode; holding the DBLock in any
    // mode is acceptable for reading it. (Note: accessing this class at all requires holding the
    // DBLock in some mode, since it requires having a pointer to the Database).

    ShardingMigrationCriticalSection _critSec;

    // This shard server's cached dbVersion. If boost::none, indicates this shard server does not
    // know the dbVersion.
    boost::optional<DatabaseVersion> _dbVersion = boost::none;
};

}  // namespace mongo
