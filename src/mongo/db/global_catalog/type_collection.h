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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
class CollectionType : private CollectionTypeBase {
public:
    // Make field names accessible.
    static constexpr auto kEpochFieldName = kPre22CompatibleEpochFieldName;

    using CollectionTypeBase::kAllowMigrationsFieldName;
    using CollectionTypeBase::kDefaultCollationFieldName;
    using CollectionTypeBase::kDefragmentationPhaseFieldName;
    using CollectionTypeBase::kDefragmentCollectionFieldName;
    using CollectionTypeBase::kEnableAutoMergeFieldName;
    using CollectionTypeBase::kKeyPatternFieldName;
    using CollectionTypeBase::kMaxChunkSizeBytesFieldName;
    using CollectionTypeBase::kNoBalanceFieldName;
    using CollectionTypeBase::kNssFieldName;
    using CollectionTypeBase::kPermitMigrationsFieldName;
    using CollectionTypeBase::kReshardingFieldsFieldName;
    using CollectionTypeBase::kTimeseriesFieldsFieldName;
    using CollectionTypeBase::kTimestampFieldName;
    using CollectionTypeBase::kUniqueFieldName;
    using CollectionTypeBase::kUnsplittableFieldName;
    using CollectionTypeBase::kUpdatedAtFieldName;
    using CollectionTypeBase::kUuidFieldName;

    // Make getters and setters accessible.
    using CollectionTypeBase::getDefragmentationPhase;
    using CollectionTypeBase::getKeyPattern;
    using CollectionTypeBase::getMaxChunkSizeBytes;
    using CollectionTypeBase::getNss;
    using CollectionTypeBase::getReshardingFields;
    using CollectionTypeBase::getTimeseriesFields;
    using CollectionTypeBase::getTimestamp;
    using CollectionTypeBase::getUnique;
    using CollectionTypeBase::getUnsplittable;
    using CollectionTypeBase::getUpdatedAt;
    using CollectionTypeBase::getUuid;
    using CollectionTypeBase::setDefragmentationPhase;
    using CollectionTypeBase::setDefragmentCollection;
    using CollectionTypeBase::setKeyPattern;
    using CollectionTypeBase::setNss;
    using CollectionTypeBase::setReshardingFields;
    using CollectionTypeBase::setTimeseriesFields;
    using CollectionTypeBase::setTimestamp;
    using CollectionTypeBase::setUnique;
    using CollectionTypeBase::setUnsplittable;
    using CollectionTypeBase::setUpdatedAt;
    using CollectionTypeBase::setUuid;
    using CollectionTypeBase::toBSON;

    // Name of the collections collection in the config server.
    static const NamespaceString ConfigNS;

    CollectionType(NamespaceString nss,
                   OID epoch,
                   Timestamp creationTime,
                   Date_t updatedAt,
                   UUID uuid,
                   KeyPattern keyPattern);

    explicit CollectionType(const BSONObj& obj);

    CollectionType() = default;

    std::string toString() const;

    const OID& getEpoch() const {
        return *getPre22CompatibleEpoch();
    }
    void setEpoch(OID epoch);

    BSONObj getDefaultCollation() const {
        return CollectionTypeBase::getDefaultCollation().get_value_or(BSONObj());
    }

    void setMaxChunkSizeBytes(int64_t value);

    void setDefaultCollation(const BSONObj& defaultCollation);

    bool getDefragmentCollection() const {
        return CollectionTypeBase::getDefragmentCollection().get_value_or(false);
    }

    bool getAllowBalance() const {
        return !getNoBalance() && !getDefragmentCollection();
    }

    bool getAllowMigrations() const {
        return CollectionTypeBase::getAllowMigrations().get_value_or(true);
    }

    void setAllowMigrations(bool allowMigrations) {
        if (allowMigrations)
            CollectionTypeBase::setAllowMigrations(boost::none);
        else
            CollectionTypeBase::setAllowMigrations(false);
    }

    // TODO SERVER-61033: remove after permitMigrations have been merge with allowMigrations.
    bool getPermitMigrations() const {
        return CollectionTypeBase::getPermitMigrations().get_value_or(true);
    }
};

}  // namespace mongo
