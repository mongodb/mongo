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

#pragma once

#include <vector>

#include "mongo/base/status_with.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

/**
 * Provides support for parsing and serialization of arguments to the config server mergeChunk
 * command.
 */
class MergeChunkRequest {
public:
    MergeChunkRequest(NamespaceString nss,
                      OID epoch,
                      std::vector<BSONObj> chunkBoundaries,
                      std::string shardName);

    /**
     * Parses the provided BSON content as the internal _configsvrMergeChunk command, and if
     * it contains the correct types, constructs a MergeChunkRequest object from it.
     *
     * {
     *   _configsvrMergeChunk: <NamespaceString nss>,
     *   collEpoch: <OID epoch>,
     *   chunkBoundaries: [
     *       <BSONObj key1>,
     *       <BSONObj key2>,
     *       ...
     *   ],
     *   shard: <string shard>
     * }
     */
    static StatusWith<MergeChunkRequest> parseFromConfigCommand(const BSONObj& cmdObj);

    /**
     * Creates a BSONObjBuilder and uses it to create and return a BSONObj from this
     * MergeChunkRequest instance. Calls appendAsConfigCommand and tacks on the passed-in
     * writeConcern.
     */
    BSONObj toConfigCommandBSON(const BSONObj& writeConcern);

    /**
     * Creates a serialized BSONObj of the internal _configsvrMergeChunk command from this
     * MergeChunkRequest instance.
     */
    void appendAsConfigCommand(BSONObjBuilder* cmdBuilder);

    const NamespaceString& getNamespace() const;
    const OID& getEpoch() const;
    const std::vector<BSONObj>& getChunkBoundaries() const;
    const std::string& getShardName() const;

private:
    /**
     * Returns a validation Status for this MergeChunkRequest instance. Performs checks for
     * valid Namespace, non-empty BSONObjs, and at least two chunks to merge as specified
     * in _chunkBoundaries.
     */
    Status _validate();

    NamespaceString _nss;
    OID _epoch;

    /**
     * Specifies the boundaries of the chunks to be merged.
     */
    std::vector<BSONObj> _chunkBoundaries;
    std::string _shardName;
};

}  // namespace mongo
