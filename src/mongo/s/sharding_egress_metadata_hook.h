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

#include "mongo/base/string_data.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class Shard;

namespace rpc {

class ShardingEgressMetadataHook : public rpc::EgressMetadataHook {
public:
    virtual ~ShardingEgressMetadataHook() = default;

    Status readReplyMetadata(const HostAndPort& replySource, const BSONObj& metadataObj) override;
    Status writeRequestMetadata(const HostAndPort& target, BSONObjBuilder* metadataBob) override;

    // These overloaded methods exist to allow ShardingConnectionHook, which is soon to be
    // deprecated, to use the logic in ShardingEgressMetadataHook instead of duplicating the
    // logic. ShardingConnectionHook must provide the replySource and target as strings rather than
    // HostAndPorts, since DBClientReplicaSet uses the hook before it decides on the actual host to
    // contact.
    Status readReplyMetadata(const StringData replySource, const BSONObj& metadataObj);
    Status writeRequestMetadata(bool shardedConnection,
                                const StringData target,
                                BSONObjBuilder* metadataBob);

protected:
    /**
     * On mongod this is a no-op.
     * On mongos it looks for $gleStats in a command's reply metadata, and fills in the
     * ClusterLastErrorInfo for this thread's associated Client with the data, if found.
     * This data will be used by subsequent GLE calls, to ensure we look for the correct write on
     * the correct PRIMARY.
     */
    virtual void _saveGLEStats(const BSONObj& metadata, StringData hostString) = 0;

    /**
     * Called by writeRequestMetadata() to find the config server optime that should be sent as part
     * of the ConfigServerMetadata.
     */
    virtual repl::OpTime _getConfigServerOpTime() = 0;

    /**
     * On config servers this is a no-op.
     * On shards and mongoses this advances the Grid's stored config server optime based on the
     * metadata in the response object from running a command.
     */
    virtual Status _advanceConfigOptimeFromShard(ShardId shardId, const BSONObj& metadataObj);
};

}  // namespace rpc
}  // namespace mongo
