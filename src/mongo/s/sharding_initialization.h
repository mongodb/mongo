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

#include <cstdint>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/stdx/functional.h"

namespace mongo {

namespace executor {
class TaskExecutor;
}  // namespace executor

class ConnectionString;
class OperationContext;
class ShardFactory;
class Status;
class ShardingCatalogClient;
class ShardingCatalogManager;
using ShardingCatalogManagerBuilder = stdx::function<std::unique_ptr<ShardingCatalogManager>(
    ShardingCatalogClient*, std::unique_ptr<executor::TaskExecutor>)>;

namespace rpc {
class ShardingEgressMetadataHook;
using ShardingEgressMetadataHookBuilder =
    stdx::function<std::unique_ptr<ShardingEgressMetadataHook>()>;
}  // namespace rpc

/**
 * Fixed process identifier for the dist lock manager running on a config server.
 */
extern const StringData kDistLockProcessIdForConfigServer;

/**
 * Generates a uniform string to be used as a process id for the distributed lock manager.
 */
std::string generateDistLockProcessId(OperationContext* txn);

/**
 * Takes in the connection string for reaching the config servers and initializes the global
 * ShardingCatalogClient, ShardingCatalogManager, ShardRegistry, and Grid objects.
 */
Status initializeGlobalShardingState(OperationContext* txn,
                                     const ConnectionString& configCS,
                                     StringData distLockProcessId,
                                     std::unique_ptr<ShardFactory> shardFactory,
                                     rpc::ShardingEgressMetadataHookBuilder hookBuilder,
                                     ShardingCatalogManagerBuilder catalogManagerBuilder);

/**
 * Tries to contact the config server and reload the shard registry and the cluster ID until it
 * succeeds or is interrupted.
 */
Status reloadShardRegistryUntilSuccess(OperationContext* txn);

}  // namespace mongo
