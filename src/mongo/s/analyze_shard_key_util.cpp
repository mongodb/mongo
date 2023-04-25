/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/analyze_shard_key_util.h"

#include "mongo/db/db_raii.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

Status validateNamespace(const NamespaceString& nss) {
    if (nss.isOnInternalDb()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against an internal collection");
    }
    if (nss.isSystem()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against a system collection");
    }
    if (nss.isFLE2StateCollection()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against an internal collection");
    }
    return Status::OK();
}

StatusWith<UUID> validateCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    if (CollectionCatalog::get(opCtx)->lookupView(opCtx, nss)) {
        return Status{ErrorCodes::CommandNotSupportedOnView, "The namespace corresponds to a view"};
    }

    AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, nss);
    if (!collection) {
        return Status{ErrorCodes::NamespaceNotFound,
                      str::stream() << "The namespace does not exist"};
    }
    if (collection->getCollectionOptions().encryptedFieldConfig.has_value()) {
        return Status{ErrorCodes::IllegalOperation,
                      str::stream() << "The collection has queryable encryption enabled"};
    }
    return collection->uuid();
}

Status validateIndexKey(const BSONObj& indexKey) {
    return validateShardKeyPattern(indexKey);
}

void uassertShardKeyValueNotContainArrays(const BSONObj& value) {
    for (const auto& element : value) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "The shard key contains an array field '" << element.fieldName()
                              << "'",
                element.type() != BSONType::Array);
    }
}

BSONObj extractReadConcern(OperationContext* opCtx) {
    return repl::ReadConcernArgs::get(opCtx).toBSONInner().removeField(
        ReadWriteConcernProvenanceBase::kSourceFieldName);
}

double round(double val, int n) {
    const double multiplier = std::pow(10.0, n);
    return std::ceil(val * multiplier) / multiplier;
}

double calculatePercentage(double part, double whole) {
    invariant(part >= 0);
    invariant(whole > 0);
    invariant(part <= whole);
    return round(part / whole * 100, kMaxNumDecimalPlaces);
}

}  // namespace analyze_shard_key
}  // namespace mongo
