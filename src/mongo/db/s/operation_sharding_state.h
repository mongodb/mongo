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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

class OperationContext;

/**
 * A decoration on OperationContext representing per-operation shard version metadata sent to mongod
 * from mongos as a command parameter.
 *
 * The metadata for a particular operation can be retrieved using the get() method.
 *
 * Note: This only supports storing the version for a single namespace.
 */
class OperationShardingState {
    MONGO_DISALLOW_COPYING(OperationShardingState);

public:
    class IgnoreVersioningBlock;

    OperationShardingState();

    /**
     * Retrieves a reference to the shard version decorating the OperationContext, 'txn'.
     */
    static OperationShardingState& get(OperationContext* txn);

    /**
     * Parses shard version from the command parameters 'cmdObj' and stores the results in this
     * object along with the give namespace that is associated with the version. Does nothing
     * if no shard version is attached to the command.
     *
     * Expects the format { ..., shardVersion: [<version>, <epoch>] }.
     *
     * This initialization may only be performed once for the lifetime of the object, which
     * coincides with the lifetime of the request.
     */
    void initializeShardVersion(NamespaceString ns, const BSONElement& shardVersionElement);

    /**
     * Returns whether or not there is a shard version associated with this operation.
     */
    bool hasShardVersion() const;

    /**
     * Returns the shard version (i.e. maximum chunk version) of a namespace being used by the
     * operation. Documents in chunks which did not belong on this shard at this shard version
     * will be filtered out.
     *
     * Returns ChunkVersion::UNSHARDED() if this operation has no shard version information
     * for the requested namespace.
     */
    const ChunkVersion& getShardVersion(const NamespaceString& ns) const;

    /**
     * Stores the given chunk version of a namespace into this object.
     */
    void setShardVersion(NamespaceString ns, ChunkVersion newVersion);

private:
    /**
     * Resets this object back as if it was default constructed (ie _hasVersion is false,
     * _shardVersion is UNSHARDED, _ns is empty).
     */
    void _clear();

    bool _hasVersion = false;
    ChunkVersion _shardVersion;
    NamespaceString _ns;
};

/**
 * RAII type that sets the shard version for the current operation to IGNORED in its constructor,
 * then restores the original version in its destructor.  Used for temporarily disabling shard
 * version checking for certain operations, such as multi-updates, that need to be unversioned
 * but may be part of a larger group of operations with a single OperationContext where the other
 * sub-operations might still require versioning.
 */
class OperationShardingState::IgnoreVersioningBlock {
    MONGO_DISALLOW_COPYING(IgnoreVersioningBlock);

public:
    IgnoreVersioningBlock(OperationContext* txn, const NamespaceString& ns);
    ~IgnoreVersioningBlock();

private:
    OperationContext* _txn;
    NamespaceString _ns;
    ChunkVersion _originalVersion;
    bool _hadOriginalVersion;
};

}  // namespace mongo
