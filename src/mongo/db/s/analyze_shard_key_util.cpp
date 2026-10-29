// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/analyze_shard_key_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/shard_role/shard_role.h"
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
