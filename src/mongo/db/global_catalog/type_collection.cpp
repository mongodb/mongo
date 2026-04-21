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
