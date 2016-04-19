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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_factory.h"

#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/connection_string.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ShardFactory::ShardFactory(std::unique_ptr<RemoteCommandTargeterFactory> targeterFactory)
    : _targeterFactory(std::move(targeterFactory)){};

std::unique_ptr<Shard> ShardFactory::createUniqueShard(const ShardId& shardId,
                                                       const ConnectionString& connStr,
                                                       bool isLocal) {
    if (isLocal) {
        // TODO: Replace with the following line once ShardLocal is implemented.
        // return stdx::make_unique<ShardLocal>(shardId);
        MONGO_UNREACHABLE;
    } else {
        return stdx::make_unique<ShardRemote>(shardId, connStr, _targeterFactory->create(connStr));
    }
}

std::shared_ptr<Shard> ShardFactory::createShard(const ShardId& shardId,
                                                 const ConnectionString& connStr,
                                                 bool isLocal) {
    if (isLocal) {
        // TODO: Replace with the following line once ShardLocal is implemented.
        // return stdx::make_shared<ShardLocal>(shardId);
        MONGO_UNREACHABLE;
    } else {
        return std::make_shared<ShardRemote>(shardId, connStr, _targeterFactory->create(connStr));
    }
}

}  // namespace mongo
