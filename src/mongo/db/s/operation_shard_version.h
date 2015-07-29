/*
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

#include "mongo/db/operation_context.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

/**
 * A decoration on OperationContext representing per-operation shard version metadata sent to mongod
 * from mongos as a command parameter.
 *
 * The metadata for a particular operation can be retrieved using the get() method.
 */
class OperationShardVersion {
public:
    /**
     * Retrieves a reference to the shard version decorating the OperationContext, 'txn'.
     */
    static OperationShardVersion& get(OperationContext* txn);

    /**
     * Parses shard version from the command parameters 'cmdObj' and stores the results in this
     * object. If no shard version is attached to the command, does nothing.
     *
     * Expects the format { ..., shardVersion: [<version>, <epoch>] }.
     */
    void initializeFromCommand(const BSONObj& cmdObj);

    /**
     * Returns whether or not there is a shard version associated with this operation.
     */
    bool hasShardVersion() const;

    /**
     * Returns the shard version (i.e. maximum chunk version) being used by the operation. Documents
     * in chunks which did not belong on this shard at this shard version will be filtered out.
     *
     * Only valid if hasShardVersion() returns true.
     */
    const ChunkVersion& getShardVersion() const;

private:
    bool _hasVersion = false;
    ChunkVersion _shardVersion;
};

}  // namespace mongo
