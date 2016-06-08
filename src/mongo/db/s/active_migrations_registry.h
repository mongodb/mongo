/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <boost/optional.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class ScopedRegisterMigration;
template <typename T>
class StatusWith;

/**
 * Thread-safe object, which keeps track of the active migrations running on a node and limits them
 * to only one per-shard. There is only one instance of this object per shard.
 */
class ActiveMigrationsRegistry {
    MONGO_DISALLOW_COPYING(ActiveMigrationsRegistry);

public:
    ActiveMigrationsRegistry();
    ~ActiveMigrationsRegistry();

    /**
     * Registers an active move chunk request with the specified arguments in order to ensure that
     * there is a single active move chunk operation running per shard.
     *
     * If there aren't any migrations running on this shard, returns a ScopedRegisterMigration
     * object, which will unregister it when it goes out of scope. Othwerwise returns
     * ConflictingOperationInProgress.
     */
    StatusWith<ScopedRegisterMigration> registerMigration(const MoveChunkRequest& args);

    /**
     * If a migration has been previously registered through a call to registerMigration returns
     * that namespace. Otherwise returns boost::none.
     */
    boost::optional<NamespaceString> getActiveMigrationNss();

private:
    friend class ScopedRegisterMigration;

    /**
     * Unregisters a previously registered namespace with ongoing migration. Must only be called if
     * a previous call to registerMigration has succeeded.
     */
    void _clearMigration();

    // Protects the state below
    stdx::mutex _mutex;

    // If there is an active moveChunk operation going on, this field contains the request, which
    // initiated it
    boost::optional<MoveChunkRequest> _activeMoveChunkRequest;
};

/**
 * RAII object, which when it goes out of scope will unregister a previously started active
 * migration from the registry it is associated with.
 */
class ScopedRegisterMigration {
    MONGO_DISALLOW_COPYING(ScopedRegisterMigration);

public:
    ScopedRegisterMigration(ActiveMigrationsRegistry* registry);
    ~ScopedRegisterMigration();

    ScopedRegisterMigration(ScopedRegisterMigration&&);
    ScopedRegisterMigration& operator=(ScopedRegisterMigration&&);

private:
    // Registry from which to unregister the migration. Not owned.
    ActiveMigrationsRegistry* _registry;
};

}  // namespace mongo
