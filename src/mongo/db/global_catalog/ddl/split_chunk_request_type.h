// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Provides support for parsing and serialization of arguments to the config server splitChunk
 * command.
 */
class SplitChunkRequest {
public:
    SplitChunkRequest(NamespaceString nss,
                      std::string shardName,
                      OID epoch,
                      boost::optional<Timestamp> timestamp,
                      ChunkRange chunkRange,
                      std::vector<BSONObj> splitPoints);

    /**
     * Parses the provided BSON content as the internal _configsvrCommitChunkSplit command, and if
     * it contains the correct types, constructs a SplitChunkRequest object from it.
     *
     * {
     *   _configsvrCommitChunkSplit: <NamespaceString nss>,
     *   collEpoch: <OID epoch>,
     *   min: <BSONObj chunkToSplitMin>,
     *   max: <BSONObj chunkToSplitMax>,
     *   splitPoints: [<BSONObj key>, ...],
     *   shard: <string shard>,
     * }
     */
    static StatusWith<SplitChunkRequest> parseFromConfigCommand(const BSONObj& cmdObj);

    /**
     * Creates a BSONObjBuilder and uses it to create and return a BSONObj from this
     * SplitChunkRequest instance. Calls appendAsConfigCommand and tacks on the passed-in
     * writeConcern.
     */
    BSONObj toConfigCommandBSON(const BSONObj& writeConcern);

    /**
     * Creates a serialized BSONObj of the internal _configsvrCommitChunkSplit command from this
     * SplitChunkRequest instance.
     */
    void appendAsConfigCommand(BSONObjBuilder* cmdBuilder);

    const NamespaceString& getNamespace() const;
    const OID& getEpoch() const;
    const auto& getTimestamp() const {
        return _timestamp;
    }
    const ChunkRange& getChunkRange() const;
    const std::vector<BSONObj>& getSplitPoints() const;
    const std::string& getShardName() const;

private:
    /**
     * Returns a validation Status for this SplitChunkRequest instance. Performs checks for
     * valid Namespace and non-empty BSONObjs.
     */
    Status _validate();

    NamespaceString _nss;
    OID _epoch;
    boost::optional<Timestamp> _timestamp;
    ChunkRange _chunkRange;
    std::vector<BSONObj> _splitPoints;
    std::string _shardName;
};

}  // namespace mongo
