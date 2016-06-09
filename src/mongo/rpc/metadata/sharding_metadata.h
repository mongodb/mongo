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

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/protocol.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class Status;
template <typename T>
class StatusWith;

namespace rpc {

/**
 * This class compromises the reply metadata fields that concern sharding. MongoD attaches
 * this information to a command reply, which MongoS uses to process getLastError.
 * TODO(spencer): Rename this to ShardingResponseMetadata.
 */
class ShardingMetadata {
public:
    /**
     * Reads ShardingMetadata from a metadata object.
     */
    static StatusWith<ShardingMetadata> readFromMetadata(const BSONObj& metadataObj);

    /**
     * Writes ShardingMetadata to a metadata builder.
     */
    Status writeToMetadata(BSONObjBuilder* metadataBob) const;

    /**
     * Rewrites the ShardingMetadata from the legacy OP_QUERY format to the metadata object
     * format.
     */
    static Status downconvert(const BSONObj& commandReply,
                              const BSONObj& replyMetadata,
                              BSONObjBuilder* legacyCommandReply);

    /**
     * Rewrites the ShardingMetadata from the legacy OP_QUERY format to the metadata object
     * format.
     */
    static Status upconvert(const BSONObj& legacyCommandReply,
                            BSONObjBuilder* commandReplyBob,
                            BSONObjBuilder* metadataBob);

    /**
     * Gets the OpTime of the oplog entry of the last successful write operation executed by the
     * server that produced the metadata.
     */
    const repl::OpTime& getLastOpTime() const;

    /**
     * Gets the most recent election id observed by the server that produced the metadata.
     */
    const OID& getLastElectionId() const;

    ShardingMetadata(repl::OpTime lastOpTime, OID lastElectionId);

private:
    repl::OpTime _lastOpTime;
    OID _lastElectionId;
};

}  // namespace rpc
}  // namespace mongo
