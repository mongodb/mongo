// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

CollectionType::CollectionType(NamespaceString nss,
                               OID epoch,
                               Timestamp creationTime,
                               Date_t updatedAt,
                               UUID uuid,
                               KeyPattern keyPattern) {
    setNss(std::move(nss));
    setUpdatedAt(std::move(updatedAt));
    setTimestamp(std::move(creationTime));
    invariant(getTimestamp() != Timestamp(0, 0));
    setUuid(std::move(uuid));
    setKeyPattern(std::move(keyPattern));
    setEpoch(std::move(epoch));
}

CollectionType::CollectionType(const BSONObj& obj) {
    *this = CollectionType::parse(obj, IDLParserContext("CollectionType"));
}

CollectionType CollectionType::parse(const BSONObj& obj, const IDLParserContext& ctx) {
    CollectionType result;
    result.parseProtected(obj, ctx);
    invariant(result.getTimestamp() != Timestamp(0, 0));
    uassert(ErrorCodes::BadValue,
            str::stream() << "Invalid namespace " << result.getNss().toStringForErrorMsg(),
            result.getNss().isValid());
    if (!result.getPre22CompatibleEpoch()) {
        result.setPre22CompatibleEpoch(OID());
    }
    return result;
}

std::string CollectionType::toString() const {
    return toBSON().toString();
}

void CollectionType::setEpoch(OID epoch) {
    setPre22CompatibleEpoch(std::move(epoch));
}

void CollectionType::setDefaultCollation(const BSONObj& defaultCollation) {
    if (!defaultCollation.isEmpty()) {
        GlobalCatalogCollectionTypeBase::setDefaultCollation(defaultCollation);
    } else {
        GlobalCatalogCollectionTypeBase::setDefaultCollation(boost::none);
    }
}

void CollectionType::setMaxChunkSizeBytes(int64_t value) {
    uassert(ErrorCodes::BadValue, "Default chunk size is out of range", value > 0);
    GlobalCatalogCollectionTypeBase::setMaxChunkSizeBytes(value);
}

BSONObj CollectionType::toShardCatalogBSON() const {
    BSONObjBuilder builder;
    getComparableFields().serialize(&builder);
    getNonComparableFields().serialize(&builder);
    return builder.obj();
}

ShardCatalogCollectionTypeBase CollectionType::asShardCatalogType() const {
    ShardCatalogCollectionTypeBase result;
    result.setComparableFields(getComparableFields());
    result.setNonComparableFields(getNonComparableFields());
    return result;
}

}  // namespace mongo
