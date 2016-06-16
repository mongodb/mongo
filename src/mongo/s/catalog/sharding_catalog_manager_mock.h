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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"

namespace mongo {

/**
 * A dummy implementation of ShardingCatalogManager for testing purposes.
 */
class ShardingCatalogManagerMock : public ShardingCatalogManager {
public:
    ShardingCatalogManagerMock();
    ~ShardingCatalogManagerMock();

    Status startup() override;

    void shutDown(OperationContext* txn) override;

    StatusWith<std::string> addShard(OperationContext* txn,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize) override;

    Status addShardToZone(OperationContext* txn,
                          const std::string& shardName,
                          const std::string& zoneName) override;

    Status removeShardFromZone(OperationContext* txn,
                               const std::string& shardName,
                               const std::string& zoneName) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) override;

    Status initializeConfigDatabaseIfNeeded(OperationContext* txn) override;

    Status upsertShardIdentityOnShard(OperationContext* txn, ShardType shardType) override;

    BSONObj createShardIdentityUpsertForAddShard(OperationContext* txn,
                                                 const std::string& shardName) override;
};

}  // namespace mongo
