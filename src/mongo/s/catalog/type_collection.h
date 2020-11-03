/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/catalog/type_collection_gen.h"

namespace mongo {

using ReshardingFields = TypeCollectionReshardingFields;

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
 *      "dropped" : false,
 *      "key" : {
 *          "_id" : 1
 *      },
 *      "defaultCollation" : {
 *          "locale" : "fr_CA"
 *      },
 *      "unique" : false,
 *      "uuid" : UUID,
 *      "noBalance" : false,
 *      "distributionMode" : "unsharded|sharded",
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
class CollectionType : private CollectionTypeBase {
public:
    // Make field names accessible.
    static constexpr auto kDefaultCollationFieldName =
        CollectionTypeBase::kPre50CompatibleDefaultCollationFieldName;
    static constexpr auto kEpochFieldName = CollectionTypeBase::kPre22CompatibleEpochFieldName;
    static constexpr auto kKeyPatternFieldName =
        CollectionTypeBase::kPre50CompatibleKeyPatternFieldName;
    static constexpr auto kUuidFieldName = CollectionTypeBase::kPre50CompatibleUuidFieldName;
    using CollectionTypeBase::kAllowMigrationsFieldName;
    using CollectionTypeBase::kNssFieldName;
    using CollectionTypeBase::kReshardingFieldsFieldName;
    using CollectionTypeBase::kUniqueFieldName;
    using CollectionTypeBase::kUpdatedAtFieldName;

    // Make getters and setters accessible.
    using CollectionTypeBase::getAllowMigrations;
    using CollectionTypeBase::getNss;
    using CollectionTypeBase::getReshardingFields;
    using CollectionTypeBase::getUnique;
    using CollectionTypeBase::getUpdatedAt;
    using CollectionTypeBase::setAllowMigrations;
    using CollectionTypeBase::setNss;
    using CollectionTypeBase::setReshardingFields;
    using CollectionTypeBase::setUnique;
    using CollectionTypeBase::setUpdatedAt;

    // Name of the collections collection in the config server.
    static const NamespaceString ConfigNS;

    static const BSONField<std::string> distributionMode;

    CollectionType() = default;

    CollectionType(NamespaceString nss, OID epoch, Date_t updatedAt, UUID uuid);

    explicit CollectionType(const BSONObj& obj);

    /**
     * Constructs a new CollectionType object from BSON. Also does validation of the contents.
     *
     * Dropped collections accumulate in the collections list, through 3.6, so that
     * mongos <= 3.4.x, when it retrieves the list from the config server, can delete its
     * cache entries for dropped collections.  See SERVER-27475, SERVER-27474
     */
    static StatusWith<CollectionType> fromBSON(const BSONObj& source);

    enum class DistributionMode {
        kUnsharded,
        kSharded,
    };

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const OID& getEpoch() const {
        return *getPre22CompatibleEpoch();
    }
    void setEpoch(OID epoch);

    const UUID& getUuid() const {
        return *getPre50CompatibleUuid();
    }
    void setUuid(UUID uuid);

    bool getDropped() const {
        return getPre50CompatibleDropped().get_value_or(false);
    }

    const KeyPattern& getKeyPattern() const {
        return *getPre50CompatibleKeyPattern();
    }
    void setKeyPattern(KeyPattern keyPattern);

    BSONObj getDefaultCollation() const {
        return getPre50CompatibleDefaultCollation().get_value_or(BSONObj());
    }
    void setDefaultCollation(const BSONObj& defaultCollation);

    bool getAllowBalance() const {
        return !getNoBalance();
    }

    DistributionMode getDistributionMode() const {
        return _distributionMode.get_value_or(DistributionMode::kSharded);
    }
    void setDistributionMode(DistributionMode distributionMode) {
        _distributionMode = distributionMode;
    }

    bool hasSameOptions(const CollectionType& other) const;

private:
    // New field in v4.4; optional in v4.4 for backwards compatibility with v4.2. Whether the
    // collection is unsharded or sharded. If missing, implies sharded.
    boost::optional<DistributionMode> _distributionMode;
};

}  // namespace mongo
