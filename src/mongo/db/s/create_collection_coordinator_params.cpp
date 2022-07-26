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

#include "mongo/db/s/create_collection_coordinator_params.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
namespace mongo {

namespace {
BSONObj resolveCollationForUserQueries(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const boost::optional<BSONObj>& collationInRequest) {
    // Ensure the collation is valid. Currently we only allow the simple collation.
    std::unique_ptr<CollatorInterface> requestedCollator = nullptr;
    if (collationInRequest) {
        requestedCollator =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(collationInRequest.value()));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: " << collationInRequest.value(),
                !requestedCollator);
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IS, AutoGetCollectionViewMode::kViewsForbidden);

    const auto actualCollator = [&]() -> const CollatorInterface* {
        const auto& coll = autoColl.getCollection();
        if (coll) {
            uassert(
                ErrorCodes::InvalidOptions, "can't shard a capped collection", !coll->isCapped());
            return coll->getDefaultCollator();
        }

        return nullptr;
    }();

    if (!requestedCollator && !actualCollator)
        return BSONObj();

    auto actualCollation = actualCollator->getSpec();
    auto actualCollatorBSON = actualCollation.toBSON();

    if (!collationInRequest) {
        auto actualCollatorFilter =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(actualCollatorBSON));
        uassert(ErrorCodes::BadValue,
                str::stream() << "If no collation was specified, the collection collation must be "
                                 "{locale: 'simple'}, "
                              << "but found: " << actualCollatorBSON,
                !actualCollatorFilter);
    }

    return actualCollatorBSON;
}
}  // namespace

CreateCollectionCoordinatorParams::CreateCollectionCoordinatorParams(
    const CreateCollectionRequest& request, const NamespaceString& targetedNamespace)
    : CreateCollectionRequest(request),
      _resolutionPerformed(false),
      _originalNamespace(targetedNamespace),
      _resolvedNamespace() {}

void CreateCollectionCoordinatorParams::resolveAgainstLocalCatalog(OperationContext* opCtx) {
    invariant(!_resolutionPerformed, "The resolution should only be performed once");
    auto bucketsNs = _originalNamespace.makeTimeseriesBucketsNamespace();
    auto existingBucketsColl =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, bucketsNs);

    auto& timeseriesOptionsInRequest = CreateCollectionRequest::getTimeseries();

    if (!timeseriesOptionsInRequest && !existingBucketsColl) {
        // The request is targeting a new or existing standard collection.
        _resolvedNamespace = _originalNamespace;
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace too long. Namespace: " << _resolvedNamespace
                              << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
                _resolvedNamespace.size() <= NamespaceString::MaxNsShardedCollectionLen);

        auto targetCollection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(
            opCtx, _resolvedNamespace);
        _resolvedCollation = resolveCollationForUserQueries(
            opCtx, _resolvedNamespace, CreateCollectionRequest::getCollation());
        _shardKeyPattern = ShardKeyPattern(*getShardKey());
        _resolutionPerformed = true;
        return;
    }

    // The request is targeting a new or existing Timeseries collection.
    _resolvedNamespace = bucketsNs;
    uassert(ErrorCodes::IllegalOperation,
            "Sharding a timeseries collection feature is not enabled",
            feature_flags::gFeatureFlagShardedTimeSeries.isEnabled(
                serverGlobalParams.featureCompatibility));

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Namespace too long. Namespace: " << _resolvedNamespace
                          << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
            _resolvedNamespace.size() <= NamespaceString::MaxNsShardedCollectionLen);

    // Consolidate the related request parameters...
    auto existingTimeseriesOptions = [&bucketsNs, &existingBucketsColl] {
        if (!existingBucketsColl) {
            return boost::optional<TimeseriesOptions>();
        }

        uassert(6159000,
                str::stream() << "the collection '" << bucketsNs
                              << "' does not have 'timeseries' options",
                existingBucketsColl->getTimeseriesOptions());
        return existingBucketsColl->getTimeseriesOptions();
    }();

    if (timeseriesOptionsInRequest && existingTimeseriesOptions) {
        uassert(
            5731500,
            str::stream() << "the 'timeseries' spec provided must match that of exists '"
                          << _originalNamespace << "' collection",
            timeseries::optionsAreEqual(*timeseriesOptionsInRequest, *existingTimeseriesOptions));
    } else if (!timeseriesOptionsInRequest) {
        setTimeseries(existingTimeseriesOptions);
    }

    // check that they are consistent with the requested shard key...
    auto timeFieldName = timeseriesOptionsInRequest->getTimeField();
    auto metaFieldName = timeseriesOptionsInRequest->getMetaField();
    BSONObjIterator shardKeyElems{*getShardKey()};
    while (auto elem = shardKeyElems.next()) {
        if (elem.fieldNameStringData() == timeFieldName) {
            uassert(5914000,
                    str::stream() << "the time field '" << timeFieldName
                                  << "' can be only at the end of the shard key pattern",
                    !shardKeyElems.more());
        } else {
            uassert(5914001,
                    str::stream() << "only the time field or meta field can be "
                                     "part of shard key pattern",
                    metaFieldName &&
                        (elem.fieldNameStringData() == *metaFieldName ||
                         elem.fieldNameStringData().startsWith(*metaFieldName + ".")));
        }
    }
    // ...and create the shard key pattern object.
    _shardKeyPattern.emplace(
        uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
            *timeseriesOptionsInRequest, *getShardKey())));

    _resolvedCollation = resolveCollationForUserQueries(
        opCtx, _resolvedNamespace, CreateCollectionRequest::getCollation());
    _resolutionPerformed = true;
}

BSONObj CreateCollectionCoordinatorParams::getResolvedCollation() const {
    invariant(_resolutionPerformed);
    return _resolvedCollation;
}

const NamespaceString& CreateCollectionCoordinatorParams::getNameSpaceToShard() const {
    invariant(_resolutionPerformed);
    return _resolvedNamespace;
}

const ShardKeyPattern& CreateCollectionCoordinatorParams::getShardKeyPattern() const {
    invariant(_resolutionPerformed);
    return *_shardKeyPattern;
}

const boost::optional<TimeseriesOptions>& CreateCollectionCoordinatorParams::getTimeseries() const {
    invariant(_resolutionPerformed);
    return CreateCollectionRequest::getTimeseries();
}

boost::optional<TimeseriesOptions>& CreateCollectionCoordinatorParams::getTimeseries() {
    invariant(_resolutionPerformed);
    return CreateCollectionRequest::getTimeseries();
}
}  // namespace mongo
