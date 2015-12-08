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

#include <boost/optional.hpp>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/optime_pair.h"
#include "mongo/stdx/memory.h"

namespace mongo {

/**
 * Common implementation shared by concrete catalog manager classes.
 */
class CatalogManagerCommon : public CatalogManager {
public:
    virtual ~CatalogManagerCommon() = default;

    Status enableSharding(OperationContext* txn, const std::string& dbName) final;

    StatusWith<std::string> addShard(OperationContext* txn,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize) final;

    Status updateDatabase(OperationContext* txn,
                          const std::string& dbName,
                          const DatabaseType& db) final;

    Status updateCollection(OperationContext* txn,
                            const std::string& collNs,
                            const CollectionType& coll) final;

    Status createDatabase(OperationContext* txn, const std::string& dbName) final;

    Status logAction(OperationContext* txn,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) final;

    Status logChange(OperationContext* txn,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) final;

protected:
    /**
     * Selects an optimal shard on which to place a newly created database from the set of
     * available shards. Will return ShardNotFound if shard could not be found.
     */
    static StatusWith<ShardId> selectShardForNewDatabase(OperationContext* txn,
                                                         ShardRegistry* shardRegistry);

    CatalogManagerCommon() = default;

private:
    /**
     * Checks that the given database name doesn't already exist in the config.databases
     * collection, including under different casing. Optional db can be passed and will
     * be set with the database details if the given dbName exists.
     *
     * Returns OK status if the db does not exist.
     * Some known errors include:
     *  NamespaceExists if it exists with the same casing
     *  DatabaseDifferCase if it exists under different casing.
     */
    virtual Status _checkDbDoesNotExist(OperationContext* txn,
                                        const std::string& dbName,
                                        DatabaseType* db) = 0;

    /**
     * Generates a unique name to be given to a newly added shard.
     */
    virtual StatusWith<std::string> _generateNewShardName(OperationContext* txn) = 0;

    /**
     * Creates the specified collection name in the config database.
     */
    virtual Status _createCappedConfigCollection(OperationContext* txn,
                                                 StringData collName,
                                                 int cappedSize) = 0;

    /**
     * Best effort method, which logs diagnostic events on the config server. If the config server
     * write fails for any reason a warning will be written to the local service log and the method
     * will return a failed status.
     *
     * @param txn Operation context in which the call is running
     * @param logCollName Which config collection to write to (excluding the database name)
     * @param what E.g. "split", "migrate" (not interpreted)
     * @param operationNS To which collection the metadata change is being applied (not interpreted)
     * @param detail Additional info about the metadata change (not interpreted)
     */
    Status _log(OperationContext* txn,
                const StringData& logCollName,
                const std::string& what,
                const std::string& operationNS,
                const BSONObj& detail);

    // Whether the logAction call should attempt to create the actionlog collection
    AtomicInt32 _actionLogCollectionCreated{0};

    // Whether the logChange call should attempt to create the changelog collection
    AtomicInt32 _changeLogCollectionCreated{0};
};

}  // namespace mongo
