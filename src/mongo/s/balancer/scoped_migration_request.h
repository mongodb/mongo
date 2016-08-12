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

#include "mongo/base/status_with.h"
#include "mongo/s/balancer/balancer_policy.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/migration_secondary_throttle_options.h"

namespace mongo {

/**
 * RAII class that handles writes to the config.migrations collection for a migration that comes
 * through the balancer.
 *
 * A migration must have an entry in the config.migrations collection so that the Balancer can
 * recover from stepdown/crash. This entry must be entered before a migration begins and then
 * removed once the migration has finished.
 *
 * This class should only be used by the Balancer!
 */
class ScopedMigrationRequest {
    MONGO_DISALLOW_COPYING(ScopedMigrationRequest);

public:
    /**
     * Deletes this migration's entry in the config.migrations collection, using majority write
     * concern. If there is a balancer stepdown/crash before the write propagates to a majority of
     * servers, that is alright because the balancer recovery process will handle it.
     *
     * If keepDocumentOnDestruct() has been called, then no attempt to remove the document is made.
     */
    ~ScopedMigrationRequest();

    ScopedMigrationRequest(ScopedMigrationRequest&& other);
    ScopedMigrationRequest& operator=(ScopedMigrationRequest&& other);

    /**
     * Inserts an unique migration entry in the config.migrations collection. If the write is
     * successful, a ScopedMigrationRequest object is returned; otherwise, the write error.
     *
     * The destructor will handle removing the document when it is no longer needed.
     */
    static StatusWith<ScopedMigrationRequest> writeMigration(OperationContext* txn,
                                                             const MigrateInfo& migrate,
                                                             const ChunkVersion& chunkVersion,
                                                             const ChunkVersion& collectionVersion);

    /**
     * Creates a ScopedMigrationRequest object without inserting a document into config.migrations.
     * The destructor will handle removing the migration document when it is no longer needed.
     *
     * This should only be used on Balancer recovery when a config.migrations document already
     * exists for the migration.
     */
    static ScopedMigrationRequest createForRecovery(OperationContext* txn,
                                                    const NamespaceString& nss,
                                                    const BSONObj& minKey);

    /**
     * Clears the operation context so that the destructor will not remove the config.migrations
     * document for the migration.
     *
     * This should only be used on the Balancer when it is interrupted and must leave entries in
     * config.migrations so that ongoing migrations can be recovered later.
     */
    void keepDocumentOnDestruct();

private:
    ScopedMigrationRequest(OperationContext* txn,
                           const NamespaceString& nss,
                           const BSONObj& minKey);

    // Need an operation context with which to do a write in the destructor.
    OperationContext* _txn;

    // ns and minkey are needed to identify the migration document when it is removed from
    // config.migrations by the destructor.
    NamespaceString _nss;
    BSONObj _minKey;
};

}  // namespace mongo
