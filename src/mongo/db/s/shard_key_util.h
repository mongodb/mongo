/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"

namespace mongo {
namespace shardkeyutil {

/**
 * Encapsulates the various steps performed when validating a proposed shard key against the
 * existing indexes for a collection when sharding a collection or refining its shard key.
 * Subclasses provide the implementation details specific to either case.
 */
class ShardKeyValidationBehaviors {
public:
    virtual ~ShardKeyValidationBehaviors() {}

    virtual std::vector<BSONObj> loadIndexes(const NamespaceString& nss) const = 0;

    virtual void verifyUsefulNonMultiKeyIndex(const NamespaceString& nss,
                                              const BSONObj& proposedKey) const = 0;

    virtual void verifyCanCreateShardKeyIndex(const NamespaceString& nss,
                                              std::string* errMsg) const = 0;

    virtual void createShardKeyIndex(const NamespaceString& nss,
                                     const BSONObj& proposedKey,
                                     const boost::optional<BSONObj>& defaultCollation,
                                     bool unique,
                                     boost::optional<TimeseriesOptions> tsOpts) const = 0;
};

/**
 * Implementation of steps for validating a shard key for shardCollection.
 */
class ValidationBehaviorsShardCollection final : public ShardKeyValidationBehaviors {
public:
    ValidationBehaviorsShardCollection(OperationContext* opCtx, const ShardId& dataShard)
        : _opCtx(opCtx),
          _localClient(std::make_unique<DBDirectClient>(opCtx)),
          _dataShard(
              uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dataShard))) {}

    std::vector<BSONObj> loadIndexes(const NamespaceString& nss) const override;

    void verifyUsefulNonMultiKeyIndex(const NamespaceString& nss,
                                      const BSONObj& proposedKey) const override;

    void verifyCanCreateShardKeyIndex(const NamespaceString& nss,
                                      std::string* errMsg) const override;

    void createShardKeyIndex(const NamespaceString& nss,
                             const BSONObj& proposedKey,
                             const boost::optional<BSONObj>& defaultCollation,
                             bool unique,
                             boost::optional<TimeseriesOptions> tsOpts) const override;

private:
    OperationContext* _opCtx;
    std::unique_ptr<DBDirectClient> _localClient;
    std::shared_ptr<Shard> _dataShard;
};

/**
 * Implementation of steps for validating a shard key for refineCollectionShardKey.
 */
class ValidationBehaviorsRefineShardKey final : public ShardKeyValidationBehaviors {
public:
    ValidationBehaviorsRefineShardKey(OperationContext* opCtx, const NamespaceString& nss);

    std::vector<BSONObj> loadIndexes(const NamespaceString& nss) const override;

    void verifyUsefulNonMultiKeyIndex(const NamespaceString& nss,
                                      const BSONObj& proposedKey) const override;

    void verifyCanCreateShardKeyIndex(const NamespaceString& nss,
                                      std::string* errMsg) const override;

    void createShardKeyIndex(const NamespaceString& nss,
                             const BSONObj& proposedKey,
                             const boost::optional<BSONObj>& defaultCollation,
                             bool unique,
                             boost::optional<TimeseriesOptions> tsOpts) const override;

private:
    OperationContext* _opCtx;

    CollectionRoutingInfo _cri;

    std::shared_ptr<Shard> _indexShard;
};

/**
 * Implementation of steps for validating a shard key for refineCollectionShardKey locally.
 */
class ValidationBehaviorsLocalRefineShardKey final : public ShardKeyValidationBehaviors {
public:
    ValidationBehaviorsLocalRefineShardKey(OperationContext* opCtx, const CollectionPtr& coll);

    std::vector<BSONObj> loadIndexes(const NamespaceString& nss) const override;

    void verifyUsefulNonMultiKeyIndex(const NamespaceString& nss,
                                      const BSONObj& proposedKey) const override;

    void verifyCanCreateShardKeyIndex(const NamespaceString& nss,
                                      std::string* errMsg) const override;

    void createShardKeyIndex(const NamespaceString& nss,
                             const BSONObj& proposedKey,
                             const boost::optional<BSONObj>& defaultCollation,
                             bool unique,
                             boost::optional<TimeseriesOptions> tsOpts) const override;

private:
    OperationContext* _opCtx;

    const CollectionPtr& _coll;
};

/**
 * Implementation of steps for validating a shard key for resharding building indexes after cloning.
 */
class ValidationBehaviorsReshardingBulkIndex final : public ShardKeyValidationBehaviors {
public:
    class RecipientStateMachineExternalState;
    ValidationBehaviorsReshardingBulkIndex();

    std::vector<BSONObj> loadIndexes(const NamespaceString& nss) const override;

    void verifyUsefulNonMultiKeyIndex(const NamespaceString& nss,
                                      const BSONObj& proposedKey) const override;

    void verifyCanCreateShardKeyIndex(const NamespaceString& nss,
                                      std::string* errMsg) const override;

    void createShardKeyIndex(const NamespaceString& nss,
                             const BSONObj& proposedKey,
                             const boost::optional<BSONObj>& defaultCollation,
                             bool unique,
                             boost::optional<TimeseriesOptions> tsOpts) const override;

    void setOpCtxAndCloneTimestamp(OperationContext* opCtx, Timestamp cloneTimestamp);

    boost::optional<BSONObj> getShardKeyIndexSpec() const;

private:
    OperationContext* _opCtx;
    Timestamp _cloneTimestamp;
    mutable boost::optional<BSONObj> _shardKeyIndexSpec;
};

/**
 * Compares the proposed shard key with the collection's existing indexes to ensure they are a legal
 * combination.
 *
 * Creates the required index if and only if (i) the collection is empty, (ii) no index on the
 * shard key exists, and (iii) the collection is not having its shard key refined.
 *
 * The proposed shard key must be validated against the set of existing indexes.
 * In particular, we must ensure the following constraints:
 *
 * 1. All existing unique indexes, except those which start with the _id index,
 *    must contain the proposed key as a prefix (uniqueness of the _id index is
 *    ensured by the _id generation process or guaranteed by the user).
 *
 * 2. If the collection is not empty or we are refining its shard key, there must exist at least one
 *    index that is "useful" for the proposed key.  A "useful" index is defined as adhering to
 *    all of the following properties:
 *         i. contains proposedKey as a prefix
 *         ii. is not a sparse index, partial index, or index with a non-simple collation
 *         iii. is not multikey (maybe lift this restriction later)
 *         iv. if a hashed index, has default seed (lift this restriction later)
 *
 * 3. If the proposed shard key is specified as unique, there must exist a useful,
 *    unique index exactly equal to the proposedKey (not just a prefix).
 *
 * 4. If the request concerns a timeseries collection, 'nss' must refer to the buckets namespace,
 *    'tsOpts' must have a value, and 'shardKeyPattern' must already be buckets-encoded.
 *    TODO (SERVER-79304): Remove 'updatedToHandleTimeseriesIndex' once 8.0 becomes last LTS. We
 *    will only rewrite the index if 'updatedToHandleTimeseriesIndex' is true.
 *
 * After validating these constraints:
 *
 * 5. If there is no useful index, and the collection is non-empty or we are refining the
 *    collection's shard key, we must fail.
 *
 * 6. If the collection is empty and we are not refining the collection's shard key, and it's
 *    still possible to create an index on the proposed key, we go ahead and do so.
 *
 * Returns true if the index has been created, false otherwise.
 */
bool validateShardKeyIndexExistsOrCreateIfPossible(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const ShardKeyPattern& shardKeyPattern,
                                                   const boost::optional<BSONObj>& defaultCollation,
                                                   bool unique,
                                                   bool enforceUniquenessCheck,
                                                   const ShardKeyValidationBehaviors& behaviors,
                                                   boost::optional<TimeseriesOptions> tsOpts,
                                                   bool updatedToHandleTimeseriesIndex);
/**
 * Compares the proposed shard key with the collection's existing indexes to ensure they are a legal
 * combination.
 *
 * Returns true if the shard key is valid and already exists. Steps 1, 2 and 3 of the previous
 * function.
 *
 */
bool validShardKeyIndexExists(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const ShardKeyPattern& shardKeyPattern,
                              const boost::optional<BSONObj>& defaultCollation,
                              bool requiresUnique,
                              const ShardKeyValidationBehaviors& behaviors,
                              std::string* errMsg = nullptr);

void validateShardKeyIsNotEncrypted(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const ShardKeyPattern& shardKeyPattern);

}  // namespace shardkeyutil
}  // namespace mongo
