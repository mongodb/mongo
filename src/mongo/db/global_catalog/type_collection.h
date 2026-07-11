// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

using ReshardingFields [[MONGO_MOD_NEEDS_REPLACEMENT]] = TypeCollectionReshardingFields;

/**
 * This class represents the layout and contents of documents contained in the config server's
 * config.collections collection. All manipulation of documents coming from that collection
 * should be done with this class.
 *
 * Expected config server config.collections collection format:
 *   {
 *      "_id" : "foo.bar",
 *      "lastmodEpoch" : ObjectId("58b6fd76132358839e409e47"),
 *      "lastmod" : ISODate("1970-02-19T17:02:47.296Z"),
 *      "key" : {
 *          "_id" : 1
 *      },
 *      "defaultCollation" : {
 *          "locale" : "fr_CA"
 *      },
 *      "unique" : false,
 *      "uuid" : UUID,
 *      "noBalance" : false,
 *      // Only populated if the collection is currently undergoing a resharding operation.
 *      "reshardingFields" : {
 *          "uuid" : UUID,
 *          "state" : CoordinatorState<kInitialized>,
 *          // Only populated if the collection is currently undergoing a resharding operation,
 *          // and this collection is the original sharded collection.
 *          "donorFields" : {
 *              "reshardingKey" : {
 *                  "_notTheID" : 1
 *              }
 *          },
 *          // Only populated if this collection is the temporary resharding collection in a
 *          // resharding operation.
 *          "recipientFields" : {
 *              "fetchTimestamp" : Timestamp(3, 4),
 *              "originalNamespace" : "foo.bar",
 *          }
 *      }
 *   }
 *
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] CollectionType : private GlobalCatalogCollectionTypeBase {
public:
    // Make field names accessible.
    static constexpr auto kEpochFieldName = kPre22CompatibleEpochFieldName;

    using GlobalCatalogCollectionTypeBase::kAllowChunkOperationsFieldName;
    using GlobalCatalogCollectionTypeBase::kAllowMigrationsFieldName;
    using GlobalCatalogCollectionTypeBase::kDefaultCollationFieldName;
    using GlobalCatalogCollectionTypeBase::kDefragmentationPhaseFieldName;
    using GlobalCatalogCollectionTypeBase::kDefragmentCollectionFieldName;
    using GlobalCatalogCollectionTypeBase::kEnableAutoMergeFieldName;
    using GlobalCatalogCollectionTypeBase::kKeyPatternFieldName;
    using GlobalCatalogCollectionTypeBase::kMaxChunkSizeBytesFieldName;
    using GlobalCatalogCollectionTypeBase::kNoBalanceFieldName;
    using GlobalCatalogCollectionTypeBase::kNssFieldName;
    using GlobalCatalogCollectionTypeBase::kPermitMigrationsFieldName;
    using GlobalCatalogCollectionTypeBase::kReshardingFieldsFieldName;
    using GlobalCatalogCollectionTypeBase::kTimeseriesFieldsFieldName;
    using GlobalCatalogCollectionTypeBase::kTimestampFieldName;
    using GlobalCatalogCollectionTypeBase::kUniqueFieldName;
    using GlobalCatalogCollectionTypeBase::kUnsplittableFieldName;
    using GlobalCatalogCollectionTypeBase::kUpdatedAtFieldName;
    using GlobalCatalogCollectionTypeBase::kUuidFieldName;

    // Make getters and setters accessible.
    using GlobalCatalogCollectionTypeBase::getComparableFields;
    using GlobalCatalogCollectionTypeBase::getDefragmentationPhase;
    using GlobalCatalogCollectionTypeBase::getKeyPattern;
    using GlobalCatalogCollectionTypeBase::getMaxChunkSizeBytes;
    using GlobalCatalogCollectionTypeBase::getNss;
    using GlobalCatalogCollectionTypeBase::getReshardingFields;
    using GlobalCatalogCollectionTypeBase::getTimeseriesFields;
    using GlobalCatalogCollectionTypeBase::getTimestamp;
    using GlobalCatalogCollectionTypeBase::getUnique;
    using GlobalCatalogCollectionTypeBase::getUnsplittable;
    using GlobalCatalogCollectionTypeBase::getUpdatedAt;
    using GlobalCatalogCollectionTypeBase::getUuid;
    using GlobalCatalogCollectionTypeBase::setDefragmentationPhase;
    using GlobalCatalogCollectionTypeBase::setDefragmentCollection;
    using GlobalCatalogCollectionTypeBase::setKeyPattern;
    using GlobalCatalogCollectionTypeBase::setNss;
    using GlobalCatalogCollectionTypeBase::setReshardingFields;
    using GlobalCatalogCollectionTypeBase::setTimeseriesFields;
    using GlobalCatalogCollectionTypeBase::setTimestamp;
    using GlobalCatalogCollectionTypeBase::setUnique;
    using GlobalCatalogCollectionTypeBase::setUnsplittable;
    using GlobalCatalogCollectionTypeBase::setUpdatedAt;
    using GlobalCatalogCollectionTypeBase::setUuid;
    using GlobalCatalogCollectionTypeBase::toBSON;

    CollectionType(NamespaceString nss,
                   OID epoch,
                   Timestamp creationTime,
                   Date_t updatedAt,
                   UUID uuid,
                   KeyPattern keyPattern);

    explicit CollectionType(const BSONObj& obj);

    CollectionType() = default;

    static CollectionType parse(const BSONObj& obj, const IDLParserContext& ctx);

    std::string toString() const;

    const OID& getEpoch() const {
        return *getPre22CompatibleEpoch();
    }
    void setEpoch(OID epoch);

    BSONObj getDefaultCollation() const {
        return GlobalCatalogCollectionTypeBase::getDefaultCollation().get_value_or(BSONObj());
    }

    void setMaxChunkSizeBytes(int64_t value);

    void setDefaultCollation(const BSONObj& defaultCollation);

    bool getDefragmentCollection() const {
        return GlobalCatalogCollectionTypeBase::getDefragmentCollection().get_value_or(false);
    }

    bool getAllowBalance() const {
        return !getNoBalance() && !getDefragmentCollection();
    }

    bool getAllowMigrations() const {
        return GlobalCatalogCollectionTypeBase::getAllowMigrations().get_value_or(true);
    }

    void setAllowMigrations(bool allowMigrations) {
        if (allowMigrations)
            GlobalCatalogCollectionTypeBase::setAllowMigrations(boost::none);
        else
            GlobalCatalogCollectionTypeBase::setAllowMigrations(false);
    }

    bool getAllowChunkOperations() const {
        return GlobalCatalogCollectionTypeBase::getAllowChunkOperations().get_value_or(true);
    }

    void setAllowChunkOperations(bool allowChunkOperations) {
        GlobalCatalogCollectionTypeBase::setAllowChunkOperations(
            allowChunkOperations ? boost::none : boost::make_optional(false));
    }

    // TODO SERVER-61033: remove after permitMigrations have been merge with allowMigrations.
    bool getPermitMigrations() const {
        return GlobalCatalogCollectionTypeBase::getPermitMigrations().get_value_or(true);
    }

    /**
     * Serializes the Collection entry for writes into the config.shard.catalog.collections
     * collection.
     */
    BSONObj toShardCatalogBSON() const;

    ShardCatalogCollectionTypeBase asShardCatalogType() const;
};

}  // namespace mongo
