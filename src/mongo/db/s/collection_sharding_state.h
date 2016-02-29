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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class BSONObj;
class CollectionMetadata;
class OperationContext;

/**
 * Contains all sharding-related runtime state for a given collection. One such object is assigned
 * to each sharded collection known on a mongod instance. A set of these objects is linked off the
 * instance's sharding state.
 *
 * Synchronization rules: In order to look-up this object in the instance's sharding map, one must
 * have some lock on the respective collection.
 */
class CollectionShardingState {
    MONGO_DISALLOW_COPYING(CollectionShardingState);

public:
    /**
     * Instantiates a new per-collection sharding state with some initial metadata.
     */
    CollectionShardingState(NamespaceString nss,
                            std::unique_ptr<CollectionMetadata> initialMetadata);
    ~CollectionShardingState();

    /**
     * Obtains the sharding state for the specified collection. If it does not exist, it will be
     * created and will remain active until the collection is dropped or unsharded.
     *
     * Must be called with some lock held on the specific collection being looked up and the
     * returned pointer should never be stored.
     */
    static CollectionShardingState* get(OperationContext* txn, const std::string& ns);

    /**
     * Returns the chunk metadata for the collection.
     */
    std::shared_ptr<CollectionMetadata> getMetadata() const {
        return _metadata;
    }

    /**
     * Set a new metadata to be used for this collection.
     */
    void setMetadata(std::shared_ptr<CollectionMetadata> newMetadata);

private:
    // Namespace to which this state belongs.
    const NamespaceString _nss;

    // Contains all the chunks associated with this collection. This value is always non-null.
    std::shared_ptr<CollectionMetadata> _metadata;
};

}  // namespace mongo
