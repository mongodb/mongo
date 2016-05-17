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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_egress_metadata_hook_for_mongos.h"

#include "mongo/db/client.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

namespace rpc {

void ShardingEgressMetadataHookForMongos::_saveGLEStats(const BSONObj& metadata,
                                                        StringData hostString) {
    if (!haveClient()) {
        // Client will be present only when write commands are used.
        return;
    }

    auto swShardingMetadata = rpc::ShardingMetadata::readFromMetadata(metadata);
    if (swShardingMetadata.getStatus() == ErrorCodes::NoSuchKey) {
        return;
    } else if (!swShardingMetadata.isOK()) {
        warning() << "Got invalid sharding metadata " << swShardingMetadata.getStatus()
                  << " metadata object was '" << metadata << "'";
        return;
    }

    auto shardConn = ConnectionString::parse(hostString.toString());

    // If we got the reply from this host, we expect that its 'hostString' must be valid.
    if (!shardConn.isOK()) {
        severe() << "got bad host string in saveGLEStats: " << hostString;
    }
    invariantOK(shardConn.getStatus());

    auto shardingMetadata = std::move(swShardingMetadata.getValue());

    auto& clientInfo = cc();
    LOG(4) << "saveGLEStats lastOpTime:" << shardingMetadata.getLastOpTime()
           << " electionId:" << shardingMetadata.getLastElectionId();

    ClusterLastErrorInfo::get(clientInfo)
        .addHostOpTime(
            shardConn.getValue(),
            HostOpTime(shardingMetadata.getLastOpTime(), shardingMetadata.getLastElectionId()));
}

repl::OpTime ShardingEgressMetadataHookForMongos::_getConfigServerOpTime() {
    return grid.configOpTime();
}

}  // namespace rpc
}  // namespace mongo
