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

#include "mongo/db/s/analyze_shard_key_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cmath>
#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

StatusWith<UUID> validateCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    const auto collectionOrView = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx, nss, AcquisitionPrerequisites::kRead, AcquisitionPrerequisites::kCanBeView));

    if (collectionOrView.isView()) {
        if (collectionOrView.getView().getViewDefinition().timeseries()) {
            return Status{ErrorCodes::IllegalOperation,
                          "Operation not supported for a timeseries collection"};
        }
        return Status{ErrorCodes::CommandNotSupportedOnView, "Operation not supported for a view"};
    }

    const auto& collection = collectionOrView.getCollection();
    if (!collection.exists()) {
        return Status{ErrorCodes::NamespaceNotFound,
                      str::stream() << "The namespace does not exist"};
    }
    if (collection.getCollectionPtr()->isTimeseriesCollection()) {
        return Status{ErrorCodes::IllegalOperation,
                      "Operation not supported for a timeseries collection"};
    }
    if (collection.getCollectionPtr()->getCollectionOptions().encryptedFieldConfig.has_value()) {
        return Status{
            ErrorCodes::IllegalOperation,
            str::stream()
                << "Operation not supported for a collection with queryable encryption enabled"};
    }

    return collection.uuid();
}

Status validateIndexKey(const BSONObj& indexKey) {
    return validateShardKeyPattern(indexKey);
}

void uassertShardKeyValueNotContainArrays(const BSONObj& value) {
    for (const auto& element : value) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "The shard key contains an array field '" << element.fieldName()
                              << "'",
                element.type() != BSONType::array);
    }
}

boost::optional<UUID> getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    auto collection = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead));
    return collection.exists() ? boost::make_optional(collection.uuid()) : boost::none;
}

bool isInternalClient(OperationContext* opCtx) {
    return (!opCtx->getClient()->session() || opCtx->getClient()->isInternalClient());
}

}  // namespace analyze_shard_key
}  // namespace mongo
