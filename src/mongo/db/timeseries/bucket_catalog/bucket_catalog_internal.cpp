/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"

#include <boost/utility/in_place_factory.hpp>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/fail_point.h"

namespace mongo::timeseries::bucket_catalog::internal {
namespace {
MONGO_FAIL_POINT_DEFINE(alwaysUseSameBucketCatalogStripe);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeReopeningBucket);
MONGO_FAIL_POINT_DEFINE(hangWaitingForConflictingPreparedBatch);

OperationId getOpId(OperationContext* opCtx, CombineWithInsertsFromOtherClients combine) {
    switch (combine) {
        case CombineWithInsertsFromOtherClients::kAllow:
            return 0;
        case CombineWithInsertsFromOtherClients::kDisallow:
            invariant(opCtx->getOpID());
            return opCtx->getOpID();
    }
    MONGO_UNREACHABLE;
}

/**
 * Updates stats to reflect the status of bucket fetches and queries based off of the FindResult
 * (which is populated when attempting to reopen a bucket).
 */
void updateBucketFetchAndQueryStats(const BucketFindResult& findResult,
                                    ExecutionStatsController& stats) {
    if (findResult.fetchedBucket) {
        if (findResult.bucketToReopen.has_value()) {
            stats.incNumBucketsFetched();
        } else {
            stats.incNumBucketFetchesFailed();
        }
    }

    if (findResult.queriedBucket) {
        if (findResult.bucketToReopen.has_value()) {
            stats.incNumBucketsQueried();
        } else {
            stats.incNumBucketQueriesFailed();
        }
    }
}

/**
 * Calculate the bucket max size constrained by the cache size and the cardinality of active
 * buckets.
 */
int32_t getCacheDerivedBucketMaxSize(StorageEngine* storageEngine, uint32_t workloadCardinality) {
    invariant(storageEngine);
    uint64_t storageCacheSize =
        static_cast<uint64_t>(storageEngine->getEngine()->getCacheSizeMB() * 1024 * 1024);

    if (!feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility) ||
        storageCacheSize == 0 || workloadCardinality == 0) {
        return INT_MAX;
    }

    uint64_t derivedMaxSize = storageCacheSize / (2 * workloadCardinality);
    uint64_t intMax = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    return std::min(derivedMaxSize, intMax);
}

/**
 * Abandons the write batch and notifies any waiters that the bucket has been cleared.
 */
void abortWriteBatch(WriteBatch& batch, const Status& status) {
    if (batch.promise.getFuture().isReady()) {
        return;
    }

    batch.promise.setError(status);
}
}  // namespace

StripeNumber getStripeNumber(const BucketKey& key) {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return key.hash % BucketCatalog::kNumberOfStripes;
}

StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc) {
    Date_t time;
    BSONElement metadata;

    if (!options.getMetaField().has_value()) {
        auto swTime = extractTime(doc, options.getTimeField());
        if (!swTime.isOK()) {
            return swTime.getStatus();
        }
        time = swTime.getValue();
    } else {
        auto swDocTimeAndMeta =
            extractTimeAndMeta(doc, options.getTimeField(), options.getMetaField().value());
        if (!swDocTimeAndMeta.isOK()) {
            return swDocTimeAndMeta.getStatus();
        }
        time = swDocTimeAndMeta.getValue().first;
        metadata = swDocTimeAndMeta.getValue().second;
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator, options.getMetaField()}};

    return {std::make_pair(key, time)};
}

const Bucket* findBucket(BucketStateRegistry& registry,
                         const Stripe& stripe,
                         WithLock,
                         const BucketId& bucketId,
                         IgnoreBucketState mode) {
    auto it = stripe.openBucketsById.find(bucketId);
    if (it != stripe.openBucketsById.end()) {
        if (mode == IgnoreBucketState::kYes) {
            return it->second.get();
        }

        if (auto state = getBucketState(registry, it->second.get());
            state && !state.value().conflictsWithInsertion()) {
            return it->second.get();
        }
    }
    return nullptr;
}

Bucket* useBucket(BucketStateRegistry& registry,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const BucketId& bucketId,
                  IgnoreBucketState mode) {
    return const_cast<Bucket*>(findBucket(registry, stripe, stripeLock, bucketId, mode));
}

Bucket* useBucketAndChangeState(BucketStateRegistry& registry,
                                Stripe& stripe,
                                WithLock stripeLock,
                                const BucketId& bucketId,
                                const BucketStateRegistry::StateChangeFn& change) {
    auto it = stripe.openBucketsById.find(bucketId);
    if (it != stripe.openBucketsById.end()) {
        if (auto state = changeBucketState(registry, it->second.get(), change);
            state && !state.value().conflictsWithInsertion()) {
            return it->second.get();
        }
    }
    return nullptr;
}

Bucket* useBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const CreationInfo& info,
                  AllowBucketCreation mode) {
    auto it = stripe.openBucketsByKey.find(info.key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return mode == AllowBucketCreation::kYes
            ? &allocateBucket(catalog, stripe, stripeLock, info)
            : nullptr;
    }

    auto& openSet = it->second;
    Bucket* bucket = nullptr;
    for (Bucket* potentialBucket : openSet) {
        if (potentialBucket->rolloverAction == RolloverAction::kNone) {
            bucket = potentialBucket;
            break;
        }
    }
    if (!bucket) {
        return mode == AllowBucketCreation::kYes
            ? &allocateBucket(catalog, stripe, stripeLock, info)
            : nullptr;
    }

    if (auto state = getBucketState(catalog.bucketStateRegistry, bucket);
        state && !state.value().conflictsWithInsertion()) {
        markBucketNotIdle(stripe, stripeLock, *bucket);
        return bucket;
    }

    abort(catalog,
          stripe,
          stripeLock,
          *bucket,
          nullptr,
          getTimeseriesBucketClearedError(bucket->bucketId.ns, bucket->bucketId.oid));

    return mode == AllowBucketCreation::kYes ? &allocateBucket(catalog, stripe, stripeLock, info)
                                             : nullptr;
}

Bucket* useAlternateBucket(BucketCatalog& catalog,
                           Stripe& stripe,
                           WithLock stripeLock,
                           const CreationInfo& info) {
    auto it = stripe.openBucketsByKey.find(info.key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return nullptr;
    }

    auto& openSet = it->second;
    // In order to potentially erase elements of the set while we iterate it (via _abort), we need
    // to advance the iterator before we call erase. This means we can't use the more
    // straightforward range iteration, and use the somewhat awkward pattern below.
    for (auto it = openSet.begin(); it != openSet.end();) {
        Bucket* potentialBucket = *it++;

        if (potentialBucket->rolloverAction == RolloverAction::kNone ||
            potentialBucket->rolloverAction == RolloverAction::kHardClose) {
            continue;
        }

        auto bucketTime = potentialBucket->minTime;
        if (info.time - bucketTime >= Seconds(*info.options.getBucketMaxSpanSeconds()) ||
            info.time < bucketTime) {
            continue;
        }

        auto state = getBucketState(catalog.bucketStateRegistry, potentialBucket);
        invariant(state);
        if (!state.value().conflictsWithInsertion()) {
            invariant(!potentialBucket->idleListEntry.has_value());
            return potentialBucket;
        }

        // Clean up the bucket if it has been cleared.
        if (state.value().isSet(BucketStateFlag::kCleared)) {
            abort(catalog,
                  stripe,
                  stripeLock,
                  *potentialBucket,
                  nullptr,
                  getTimeseriesBucketClearedError(potentialBucket->bucketId.ns,
                                                  potentialBucket->bucketId.oid));
        }
    }

    return nullptr;
}

StatusWith<std::unique_ptr<Bucket>> rehydrateBucket(
    OperationContext* opCtx,
    BucketStateRegistry& registry,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BucketToReopen& bucketToReopen,
    const BucketKey* expectedKey) {
    invariant(feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility));
    const auto& [bucketDoc, validator, catalogEra] = bucketToReopen;
    if (catalogEra < getCurrentEra(registry)) {
        return {ErrorCodes::WriteConflict, "Bucket is from an earlier era, may be outdated"};
    }

    BSONElement bucketIdElem = bucketDoc.getField(kBucketIdFieldName);
    if (bucketIdElem.eoo() || bucketIdElem.type() != BSONType::jstOID) {
        return {ErrorCodes::BadValue,
                str::stream() << kBucketIdFieldName << " is missing or not an ObjectId"};
    }

    // Validate the bucket document against the schema.
    auto result = validator(opCtx, bucketDoc);
    if (result.first != Collection::SchemaValidationResult::kPass) {
        return result.second;
    }

    auto controlField = bucketDoc.getObjectField(kBucketControlFieldName);
    auto closedElem = controlField.getField(kBucketControlClosedFieldName);
    if (closedElem.booleanSafe()) {
        return {ErrorCodes::BadValue,
                "Bucket has been marked closed and is not eligible for reopening"};
    }

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(kBucketMetaFieldName);
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator, options.getMetaField()}};
    if (expectedKey && key != *expectedKey) {
        return {ErrorCodes::BadValue, "Bucket metadata does not match (hash collision)"};
    }

    auto minTime = controlField.getObjectField(kBucketControlMinFieldName)
                       .getField(options.getTimeField())
                       .Date();
    BucketId bucketId{key.ns, bucketIdElem.OID()};
    std::unique_ptr<Bucket> bucket =
        std::make_unique<Bucket>(bucketId, key, options.getTimeField(), minTime, registry);

    const bool isCompressed = isCompressedBucket(bucketDoc);

    // Initialize the remaining member variables from the bucket document.
    if (isCompressed) {
        auto decompressed = decompressBucket(bucketDoc);
        if (!decompressed.has_value()) {
            return Status{ErrorCodes::BadValue, "Bucket could not be decompressed"};
        }
        bucket->size = decompressed.value().objsize();
        bucket->decompressed = DecompressionResult{bucketDoc, decompressed.value()};
        bucket->memoryUsage += (decompressed.value().objsize() + bucketDoc.objsize());
    } else {
        bucket->size = bucketDoc.objsize();
    }

    // Populate the top-level data field names.
    const BSONObj& dataObj = bucketDoc.getObjectField(kBucketDataFieldName);
    for (const BSONElement& dataElem : dataObj) {
        auto hashedKey = StringSet::hasher().hashed_key(dataElem.fieldName());
        bucket->fieldNames.emplace(hashedKey);
    }

    auto swMinMax = generateMinMaxFromBucketDoc(bucketDoc, comparator);
    if (!swMinMax.isOK()) {
        return swMinMax.getStatus();
    }
    bucket->minmax = std::move(swMinMax.getValue());

    auto swSchema = generateSchemaFromBucketDoc(bucketDoc, comparator);
    if (!swSchema.isOK()) {
        return swSchema.getStatus();
    }
    bucket->schema = std::move(swSchema.getValue());

    uint32_t numMeasurements = 0;
    const BSONElement timeColumnElem = dataObj.getField(options.getTimeField());

    if (isCompressed && timeColumnElem.type() == BSONType::BinData) {
        BSONColumn storage{timeColumnElem};
        numMeasurements = storage.size();
    } else if (timeColumnElem.isABSONObj()) {
        numMeasurements = timeColumnElem.Obj().nFields();
    } else {
        return {ErrorCodes::BadValue,
                "Bucket data field is malformed (missing a valid time column)"};
    }

    bucket->numMeasurements = numMeasurements;
    bucket->numCommittedMeasurements = numMeasurements;

    // The namespace is stored two times: the bucket itself and openBucketsByKey. We don't have a
    // great approximation for the _schema or _minmax data structure size, so we use the control
    // field size as an approximation for _minmax, and half that size for _schema. Since the
    // metadata is stored in the bucket, we need to add that as well. A unique pointer to the bucket
    // is stored once: openBucketsById. A raw pointer to the bucket is stored at most twice:
    // openBucketsByKey, idleBuckets.
    bucket->memoryUsage += (key.ns.size() * 2) + 1.5 * controlField.objsize() +
        key.metadata.toBSON().objsize() + sizeof(Bucket) + sizeof(std::unique_ptr<Bucket>) +
        (sizeof(Bucket*) * 2);

    return {std::move(bucket)};
}

StatusWith<std::reference_wrapper<Bucket>> reopenBucket(BucketCatalog& catalog,
                                                        Stripe& stripe,
                                                        WithLock stripeLock,
                                                        ExecutionStatsController& stats,
                                                        const BucketKey& key,
                                                        std::unique_ptr<Bucket>&& bucket,
                                                        std::uint64_t targetEra,
                                                        ClosedBuckets& closedBuckets) {
    invariant(bucket);

    expireIdleBuckets(catalog, stripe, stripeLock, stats, closedBuckets);

    // We may need to initialize the bucket's state.
    bool conflicts = false;
    auto initializeStateFn =
        [targetEra, &conflicts](boost::optional<BucketState> input,
                                std::uint64_t currentEra) -> boost::optional<BucketState> {
        if (targetEra < currentEra ||
            (input.has_value() && input.value().conflictsWithReopening())) {
            conflicts = true;
            return input;
        }
        conflicts = false;
        return input.has_value() ? input.value() : BucketState{};
    };

    auto state =
        changeBucketState(catalog.bucketStateRegistry, bucket->bucketId, initializeStateFn);
    if (conflicts) {
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // If this bucket was archived, we need to remove it from the set of archived buckets.
    if (auto setIt = stripe.archivedBuckets.find(key.hash); setIt != stripe.archivedBuckets.end()) {
        auto& archivedSet = setIt->second;
        if (auto bucketIt = archivedSet.find(bucket->minTime);
            bucketIt != archivedSet.end() && bucket->bucketId == bucketIt->second.bucketId) {
            long long memory =
                marginalMemoryUsageForArchivedBucket(bucketIt->second, archivedSet.size() == 1);
            if (archivedSet.size() == 1) {
                stripe.archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(bucketIt);
            }
            catalog.memoryUsage.fetchAndSubtract(memory);
            catalog.numberOfActiveBuckets.fetchAndSubtract(1);
        }
    }

    // Pass ownership of the reopened bucket to the bucket catalog.
    auto [insertedIt, newlyInserted] =
        stripe.openBucketsById.try_emplace(bucket->bucketId, std::move(bucket));
    invariant(newlyInserted);
    Bucket* unownedBucket = insertedIt->second.get();

    // If we already have an open bucket for this key, we need to close it.
    if (auto it = stripe.openBucketsByKey.find(key); it != stripe.openBucketsByKey.end()) {
        auto& openSet = it->second;
        for (Bucket* existingBucket : openSet) {
            if (existingBucket->rolloverAction == RolloverAction::kNone) {
                stats.incNumBucketsClosedDueToReopening();
                if (allCommitted(*existingBucket)) {
                    closeOpenBucket(catalog, stripe, stripeLock, *existingBucket, closedBuckets);
                } else {
                    existingBucket->rolloverAction = RolloverAction::kSoftClose;
                }
                // We should only have one open bucket at a time.
                break;
            }
        }
    }

    // Now actually mark this bucket as open.
    stripe.openBucketsByKey[key].emplace(unownedBucket);
    stats.incNumBucketsReopened();

    catalog.memoryUsage.addAndFetch(unownedBucket->memoryUsage);
    catalog.numberOfActiveBuckets.fetchAndAdd(1);

    return *unownedBucket;
}

StatusWith<std::reference_wrapper<Bucket>> reuseExistingBucket(BucketCatalog& catalog,
                                                               Stripe& stripe,
                                                               WithLock stripeLock,
                                                               ExecutionStatsController& stats,
                                                               const BucketKey& key,
                                                               Bucket& existingBucket,
                                                               std::uint64_t targetEra) {
    // If we have an existing bucket, passing the Bucket* will let us check if the bucket was
    // cleared as part of a set since the last time it was used. If we were to just check by
    // OID, we may miss if e.g. there was a move chunk operation.
    bool conflicts = false;
    auto state = changeBucketState(
        catalog.bucketStateRegistry,
        &existingBucket,
        [targetEra, &conflicts](boost::optional<BucketState> input,
                                std::uint64_t currentEra) -> boost::optional<BucketState> {
            if (targetEra < currentEra ||
                (input.has_value() && input.value().conflictsWithReopening())) {
                conflicts = true;
                return input;
            }
            conflicts = false;
            return input.has_value() ? input.value() : BucketState{};
        });
    if (state.has_value() && state.value().isSet(BucketStateFlag::kCleared)) {
        abort(catalog,
              stripe,
              stripeLock,
              existingBucket,
              nullptr,
              getTimeseriesBucketClearedError(existingBucket.bucketId.ns,
                                              existingBucket.bucketId.oid));
        conflicts = true;
    }
    if (conflicts) {
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // It's possible to have two buckets with the same ID in different collections, so let's make
    // extra sure the existing bucket is the right one.
    if (existingBucket.bucketId.ns != key.ns) {
        return {ErrorCodes::BadValue, "Cannot re-use bucket: same ID but different namespace"};
    }

    // If the bucket was already open, wasn't cleared, the state didn't conflict with reopening, and
    // the namespace matches, then we can simply return it.
    stats.incNumDuplicateBucketsReopened();
    markBucketNotIdle(stripe, stripeLock, existingBucket);

    return existingBucket;
}

stdx::variant<std::shared_ptr<WriteBatch>, RolloverReason> insertIntoBucket(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    StripeNumber stripeNumber,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine,
    AllowBucketCreation mode,
    CreationInfo& info,
    Bucket& existingBucket) {
    Bucket::NewFieldNames newFieldNamesToBeInserted;
    int32_t sizeToBeAdded = 0;

    bool isNewlyOpenedBucket = (existingBucket.size == 0);
    std::reference_wrapper<Bucket> bucketToUse{existingBucket};
    if (!isNewlyOpenedBucket) {
        auto [action, reason] = determineRolloverAction(opCtx,
                                                        doc,
                                                        info,
                                                        existingBucket,
                                                        catalog.numberOfActiveBuckets.load(),
                                                        newFieldNamesToBeInserted,
                                                        sizeToBeAdded,
                                                        mode);
        if ((action == RolloverAction::kSoftClose || action == RolloverAction::kArchive) &&
            mode == AllowBucketCreation::kNo) {
            // We don't actually want to roll this bucket over yet, bail out.
            return reason;
        } else if (action != RolloverAction::kNone) {
            info.openedDuetoMetadata = false;
            bucketToUse = rollover(catalog, stripe, stripeLock, existingBucket, info, action);
            isNewlyOpenedBucket = true;
        }
    }
    Bucket& bucket = bucketToUse.get();
    const auto previousMemoryUsage = bucket.memoryUsage;

    if (isNewlyOpenedBucket) {
        calculateBucketFieldsAndSizeChange(
            bucket, doc, info.options.getMetaField(), newFieldNamesToBeInserted, sizeToBeAdded);
    }

    auto batch = activeBatch(bucket, getOpId(opCtx, combine), stripeNumber, info.stats);
    batch->measurements.push_back(doc);
    for (auto&& field : newFieldNamesToBeInserted) {
        batch->newFieldNamesToBeInserted[field] = field.hash();
        bucket.uncommittedFieldNames.emplace(field);
    }

    bucket.numMeasurements++;
    bucket.size += sizeToBeAdded;
    if (isNewlyOpenedBucket) {
        // The namespace is stored two times: the bucket itself and openBucketsByKey.
        // We don't have a great approximation for the
        // _schema size, so we use initial document size minus metadata as an approximation. Since
        // the metadata itself is stored once, in the bucket, we can combine the two and just use
        // the initial document size. A unique pointer to the bucket is stored once:
        // openBucketsById. A raw pointer to the bucket is stored at most twice: openBucketsByKey,
        // idleBuckets.
        bucket.memoryUsage += (info.key.ns.size() * 2) + doc.objsize() + sizeof(Bucket) +
            sizeof(std::unique_ptr<Bucket>) + (sizeof(Bucket*) * 2);

        auto updateStatus = bucket.schema.update(
            doc, info.options.getMetaField(), info.key.metadata.getComparator());
        invariant(updateStatus == Schema::UpdateStatus::Updated);
    } else {
        catalog.memoryUsage.fetchAndSubtract(previousMemoryUsage);
    }
    catalog.memoryUsage.fetchAndAdd(bucket.memoryUsage);

    return batch;
}

StatusWith<InsertResult> insert(OperationContext* opCtx,
                                BucketCatalog& catalog,
                                const NamespaceString& ns,
                                const StringData::ComparatorInterface* comparator,
                                const TimeseriesOptions& options,
                                const BSONObj& doc,
                                CombineWithInsertsFromOtherClients combine,
                                AllowBucketCreation mode,
                                BucketFindResult bucketFindResult) {
    invariant(!ns.isTimeseriesBucketsCollection());

    auto res = extractBucketingParameters(ns, comparator, options, doc);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto& key = res.getValue().first;
    auto time = res.getValue().second;

    ExecutionStatsController stats = getOrInitializeExecutionStats(catalog, ns);
    updateBucketFetchAndQueryStats(bucketFindResult, stats);

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto stripeNumber = getStripeNumber(key);

    InsertResult result;
    result.catalogEra = getCurrentEra(catalog.bucketStateRegistry);
    CreationInfo info{key, stripeNumber, time, options, stats, &result.closedBuckets};
    boost::optional<BucketToReopen> bucketToReopen = std::move(bucketFindResult.bucketToReopen);

    auto rehydratedBucket = bucketToReopen.has_value()
        ? rehydrateBucket(opCtx,
                          catalog.bucketStateRegistry,
                          ns,
                          comparator,
                          options,
                          bucketToReopen.value(),
                          &key)
        : StatusWith<std::unique_ptr<Bucket>>{ErrorCodes::BadValue, "No bucket to rehydrate"};
    if (rehydratedBucket.getStatus().code() == ErrorCodes::WriteConflict) {
        stats.incNumBucketReopeningsFailed();
        return rehydratedBucket.getStatus();
    }

    auto& stripe = catalog.stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    if (rehydratedBucket.isOK()) {
        invariant(mode == AllowBucketCreation::kYes);
        hangTimeseriesInsertBeforeReopeningBucket.pauseWhileSet();

        StatusWith<std::reference_wrapper<Bucket>> swBucket{ErrorCodes::BadValue, ""};
        auto existingIt = stripe.openBucketsById.find(rehydratedBucket.getValue()->bucketId);
        if (existingIt != stripe.openBucketsById.end()) {
            // First let's check the existing bucket if we have one.
            Bucket* existingBucket = existingIt->second.get();
            swBucket = reuseExistingBucket(catalog,
                                           stripe,
                                           stripeLock,
                                           stats,
                                           key,
                                           *existingBucket,
                                           bucketToReopen->catalogEra);
        } else {
            // No existing bucket to use, go ahead and try to reopen our rehydrated bucket.
            swBucket = reopenBucket(catalog,
                                    stripe,
                                    stripeLock,
                                    stats,
                                    key,
                                    std::move(rehydratedBucket.getValue()),
                                    bucketToReopen->catalogEra,
                                    result.closedBuckets);
        }

        if (swBucket.isOK()) {
            Bucket& bucket = swBucket.getValue().get();
            auto insertionResult = insertIntoBucket(
                opCtx, catalog, stripe, stripeLock, stripeNumber, doc, combine, mode, info, bucket);
            auto* batch = stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
            invariant(batch);
            result.batch = *batch;

            return std::move(result);
        } else {
            stats.incNumBucketReopeningsFailed();
            if (swBucket.getStatus().code() == ErrorCodes::WriteConflict) {
                return swBucket.getStatus();
            }
            // If we had a different type of error, then we should fall through and proceed to open
            // a new bucket.
        }
    }

    Bucket* bucket = useBucket(catalog, stripe, stripeLock, info, mode);
    if (!bucket) {
        invariant(mode == AllowBucketCreation::kNo);
        constexpr bool allowQueryBasedReopening = true;
        result.candidate = getReopeningCandidate(
            opCtx, catalog, stripe, stripeLock, info, allowQueryBasedReopening);
        return std::move(result);
    }

    auto insertionResult = insertIntoBucket(
        opCtx, catalog, stripe, stripeLock, stripeNumber, doc, combine, mode, info, *bucket);
    if (auto* reason = stdx::get_if<RolloverReason>(&insertionResult)) {
        invariant(mode == AllowBucketCreation::kNo);
        if (allCommitted(*bucket)) {
            markBucketIdle(stripe, stripeLock, *bucket);
        }

        // If we were time forward or backward, we might be able to "reopen" a bucket we still have
        // in memory that's set to be closed when pending operations finish.
        if ((*reason == RolloverReason::kTimeBackward || *reason == RolloverReason::kTimeForward)) {
            if (Bucket* alternate = useAlternateBucket(catalog, stripe, stripeLock, info)) {
                insertionResult = insertIntoBucket(opCtx,
                                                   catalog,
                                                   stripe,
                                                   stripeLock,
                                                   stripeNumber,
                                                   doc,
                                                   combine,
                                                   mode,
                                                   info,
                                                   *alternate);
                if (auto* batch = stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
                    result.batch = *batch;
                    return std::move(result);
                }

                // We weren't able to insert into the other bucket, so fall through to the regular
                // reopening procedure.
            }
        }

        bool allowQueryBasedReopening = (*reason == RolloverReason::kTimeBackward);
        result.candidate = getReopeningCandidate(
            opCtx, catalog, stripe, stripeLock, info, allowQueryBasedReopening);
    } else {
        result.batch = *stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    }
    return std::move(result);
}

void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        std::shared_ptr<WriteBatch> current;

        {
            stdx::lock_guard stripeLock{stripe.mutex};
            Bucket* bucket = useBucket(
                registry, stripe, stripeLock, batch->bucketHandle.bucketId, IgnoreBucketState::kNo);
            if (!bucket || isWriteBatchFinished(*batch)) {
                return;
            }

            current = bucket->preparedBatch;
            if (!current) {
                // No other batches for this bucket are currently committing, so we can proceed.
                bucket->preparedBatch = batch;
                bucket->batches.erase(batch->opId);
                return;
            }
        }

        // We only hit this failpoint when there are conflicting prepared batches on the same
        // bucket.
        hangWaitingForConflictingPreparedBatch.pauseWhileSet();

        // We have to wait for someone else to finish.
        getWriteBatchResult(*current).getStatus().ignore();  // We don't care about the result.
    }
}

void removeBucket(
    BucketCatalog& catalog, Stripe& stripe, WithLock stripeLock, Bucket& bucket, RemovalMode mode) {
    invariant(bucket.batches.empty());
    invariant(!bucket.preparedBatch);

    auto allIt = stripe.openBucketsById.find(bucket.bucketId);
    invariant(allIt != stripe.openBucketsById.end());

    catalog.memoryUsage.fetchAndSubtract(bucket.memoryUsage);
    markBucketNotIdle(stripe, stripeLock, bucket);

    // If the bucket was rolled over, then there may be a different open bucket for this metadata.
    auto openIt = stripe.openBucketsByKey.find({bucket.bucketId.ns, bucket.key.metadata});
    if (openIt != stripe.openBucketsByKey.end()) {
        auto& openSet = openIt->second;
        auto bucketIt = openSet.find(&bucket);
        if (bucketIt != openSet.end()) {
            if (openSet.size() == 1) {
                stripe.openBucketsByKey.erase(openIt);
            } else {
                openSet.erase(bucketIt);
            }
        }
    }

    // If we are cleaning up while archiving a bucket, then we want to preserve its state. Otherwise
    // we can remove the state from the catalog altogether.
    switch (mode) {
        case RemovalMode::kClose: {
            auto state = getBucketState(catalog.bucketStateRegistry, bucket.bucketId);
            invariant(state.has_value());
            invariant(state.value().isSet(BucketStateFlag::kPendingCompression));
            break;
        }
        case RemovalMode::kAbort:
            changeBucketState(catalog.bucketStateRegistry,
                              bucket.bucketId,
                              [](boost::optional<BucketState> input,
                                 std::uint64_t) -> boost::optional<BucketState> {
                                  invariant(input.has_value());
                                  if (input->conflictsWithReopening()) {
                                      return input.value().setFlag(BucketStateFlag::kUntracked);
                                  }
                                  return boost::none;
                              });
            break;
        case RemovalMode::kArchive:
            // No state change
            break;
    }

    catalog.numberOfActiveBuckets.fetchAndSubtract(1);
    stripe.openBucketsById.erase(allIt);
}

void archiveBucket(BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   Bucket& bucket,
                   ClosedBuckets& closedBuckets) {
    bool archived = false;
    auto& archivedSet = stripe.archivedBuckets[bucket.key.hash];
    auto it = archivedSet.find(bucket.minTime);
    if (it == archivedSet.end()) {
        auto [it, inserted] =
            archivedSet.emplace(bucket.minTime, ArchivedBucket{bucket.bucketId, bucket.timeField});

        long long memory =
            marginalMemoryUsageForArchivedBucket(it->second, archivedSet.size() == 1);
        catalog.memoryUsage.fetchAndAdd(memory);
        archived = true;
    }

    if (archived) {
        // If we have an archived bucket, we still want to account for it in numberOfActiveBuckets
        // so we will increase it here since removeBucket decrements the count.
        catalog.numberOfActiveBuckets.fetchAndAdd(1);
        removeBucket(catalog, stripe, stripeLock, bucket, RemovalMode::kArchive);
    } else {
        // We had a meta hash collision, and already have a bucket archived with the same meta hash
        // and timestamp as this bucket. Since it's somewhat arbitrary which bucket we keep, we'll
        // keep the one that's already archived and just plain close this one.
        closeOpenBucket(catalog, stripe, stripeLock, bucket, closedBuckets);
    }
}

boost::optional<OID> findArchivedCandidate(BucketCatalog& catalog,
                                           Stripe& stripe,
                                           WithLock stripeLock,
                                           const CreationInfo& info) {
    auto setIt = stripe.archivedBuckets.find(info.key.hash);
    if (setIt == stripe.archivedBuckets.end()) {
        return boost::none;
    }

    auto& archivedSet = setIt->second;

    // We want to find the largest time that is not greater than info.time. Generally lower_bound
    // will return the smallest element not less than the search value, but we are using
    // std::greater instead of std::less for the map's comparisons. This means the order of keys
    // will be reversed, and lower_bound will return what we want.
    auto it = archivedSet.lower_bound(info.time);
    if (it == archivedSet.end()) {
        return boost::none;
    }

    const auto& [candidateTime, candidateBucket] = *it;
    invariant(candidateTime <= info.time);
    // We need to make sure our measurement can fit without violating max span. If not, we
    // can't use this bucket.
    if (info.time - candidateTime < Seconds(*info.options.getBucketMaxSpanSeconds())) {
        auto state = getBucketState(catalog.bucketStateRegistry, candidateBucket.bucketId);
        if (state && !state.value().conflictsWithReopening()) {
            return candidateBucket.bucketId.oid;
        } else {
            if (state) {
                changeBucketState(catalog.bucketStateRegistry,
                                  candidateBucket.bucketId,
                                  [](boost::optional<BucketState> input,
                                     std::uint64_t) -> boost::optional<BucketState> {
                                      if (!input.has_value()) {
                                          return boost::none;
                                      }
                                      invariant(input.value().conflictsWithReopening());
                                      return input.value().setFlag(BucketStateFlag::kUntracked);
                                  });
            }
            long long memory =
                marginalMemoryUsageForArchivedBucket(candidateBucket, archivedSet.size() == 1);
            if (archivedSet.size() == 1) {
                stripe.archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(it);
            }
            catalog.memoryUsage.fetchAndSubtract(memory);
            catalog.numberOfActiveBuckets.fetchAndSubtract(1);
        }
    }

    return boost::none;
}

stdx::variant<std::monostate, OID, std::vector<BSONObj>> getReopeningCandidate(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    const CreationInfo& info,
    bool allowQueryBasedReopening) {
    if (auto archived = findArchivedCandidate(catalog, stripe, stripeLock, info)) {
        return archived.value();
    }

    if (!allowQueryBasedReopening) {
        return {};
    }

    boost::optional<BSONElement> metaElement;
    if (info.options.getMetaField().has_value()) {
        metaElement = info.key.metadata.element();
    }

    auto controlMinTimePath = kControlMinFieldNamePrefix.toString() + info.options.getTimeField();
    auto maxDataTimeFieldPath = kDataFieldNamePrefix.toString() + info.options.getTimeField() +
        "." + std::to_string(gTimeseriesBucketMaxCount - 1);

    // Derive the maximum bucket size.
    auto bucketMaxSize = getCacheDerivedBucketMaxSize(
        opCtx->getServiceContext()->getStorageEngine(), catalog.numberOfActiveBuckets.load());
    int32_t effectiveMaxSize = std::min(gTimeseriesBucketMaxSize, bucketMaxSize);

    return generateReopeningPipeline(opCtx,
                                     info.time,
                                     metaElement,
                                     controlMinTimePath,
                                     maxDataTimeFieldPath,
                                     *info.options.getBucketMaxSpanSeconds(),
                                     effectiveMaxSize);
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           std::shared_ptr<WriteBatch> batch,
           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket = useBucket(catalog.bucketStateRegistry,
                               stripe,
                               stripeLock,
                               batch->bucketHandle.bucketId,
                               IgnoreBucketState::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        abortWriteBatch(*batch, status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    abort(catalog, stripe, stripeLock, *bucket, batch, status);
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           Bucket& bucket,
           std::shared_ptr<WriteBatch> batch,
           const Status& status) {
    // Abort any unprepared batches. This should be safe since we have a lock on the stripe,
    // preventing anyone else from using these.
    for (const auto& [_, current] : bucket.batches) {
        abortWriteBatch(*current, status);
    }
    bucket.batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the one we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket.preparedBatch) {
        if (batch && prepared == batch) {
            // We own the prepared batch, so we can go ahead and abort it and remove the bucket.
            abortWriteBatch(*prepared, status);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    if (doRemove) {
        removeBucket(catalog, stripe, stripeLock, bucket, RemovalMode::kAbort);
    } else {
        changeBucketState(
            catalog.bucketStateRegistry,
            bucket.bucketId,
            [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
                invariant(input.has_value());
                return input.value().setFlag(BucketStateFlag::kCleared);
            });
    }
}

void markBucketIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket) {
    invariant(!bucket.idleListEntry.has_value());
    invariant(allCommitted(bucket));
    stripe.idleBuckets.push_front(&bucket);
    bucket.idleListEntry = stripe.idleBuckets.begin();
}

void markBucketNotIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket) {
    if (bucket.idleListEntry.has_value()) {
        stripe.idleBuckets.erase(bucket.idleListEntry.value());
        bucket.idleListEntry = boost::none;
    }
}

void expireIdleBuckets(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       ExecutionStatsController& stats,
                       ClosedBuckets& closedBuckets) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numExpired = 0;

    const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility);

    while (!stripe.idleBuckets.empty() &&
           catalog.memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe.idleBuckets.back();

        auto state = getBucketState(catalog.bucketStateRegistry, bucket);
        if (canArchive && state && !state.value().conflictsWithInsertion()) {
            // Can archive a bucket if it's still eligible for insertions.
            archiveBucket(catalog, stripe, stripeLock, *bucket, closedBuckets);
            stats.incNumBucketsArchivedDueToMemoryThreshold();
        } else if (state && state.value().isSet(BucketStateFlag::kCleared)) {
            // Bucket was cleared and just needs to be removed from catalog.
            removeBucket(catalog, stripe, stripeLock, *bucket, RemovalMode::kAbort);
        } else {
            closeOpenBucket(catalog, stripe, stripeLock, *bucket, closedBuckets);
            stats.incNumBucketsClosedDueToMemoryThreshold();
        }

        ++numExpired;
    }

    while (canArchive && !stripe.archivedBuckets.empty() &&
           catalog.memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {

        auto& [hash, archivedSet] = *stripe.archivedBuckets.begin();
        invariant(!archivedSet.empty());

        auto& [timestamp, bucket] = *archivedSet.begin();
        closeArchivedBucket(catalog.bucketStateRegistry, bucket, closedBuckets);

        long long memory = marginalMemoryUsageForArchivedBucket(bucket, archivedSet.size() == 1);
        if (archivedSet.size() == 1) {
            // If this is the only entry, erase the whole map so we don't leave it empty.
            stripe.archivedBuckets.erase(stripe.archivedBuckets.begin());
        } else {
            // Otherwise just erase this bucket from the map.
            archivedSet.erase(archivedSet.begin());
        }
        catalog.memoryUsage.fetchAndSubtract(memory);
        catalog.numberOfActiveBuckets.fetchAndSubtract(1);

        stats.incNumBucketsClosedDueToMemoryThreshold();
        ++numExpired;
    }
}

std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options) {
    OID oid = OID::gen();

    // We round the measurement timestamp down to the nearest minute, hour, or day depending on the
    // granularity. We do this for two reasons. The first is so that if measurements come in
    // slightly out of order, we don't have to close the current bucket due to going backwards in
    // time. The second, and more important reason, is so that we reliably group measurements
    // together into predictable chunks for sharding. This way we know from a measurement timestamp
    // what the bucket timestamp will be, so we can route measurements to the right shard chunk.
    auto roundedTime = roundTimestampToGranularity(time, options);
    int64_t const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    oid.setTimestamp(roundedSeconds);

    // Now, if we stopped here we could end up with bucket OID collisions. Consider the case where
    // we have the granularity set to 'Hours'. This means we will round down to the nearest day, so
    // any bucket generated on the same machine on the same day will have the same timestamp portion
    // and unique instance portion of the OID. Only the increment will differ. Since we only use 3
    // bytes for the increment portion, we run a serious risk of overflow if we are generating lots
    // of buckets.
    //
    // To address this, we'll take the difference between the actual timestamp and the rounded
    // timestamp and add it to the instance portion of the OID to ensure we can't have a collision.
    // for timestamps generated on the same machine.
    //
    // This leaves open the possibility that in the case of step-down/step-up, we could get a
    // collision if the old primary and the new primary have unique instance bits that differ by
    // less than the maximum rounding difference. This is quite unlikely though, and can be resolved
    // by restarting the new primary. It remains an open question whether we can fix this in a
    // better way.
    // TODO (SERVER-61412): Avoid time-series bucket OID collisions after election
    auto instance = oid.getInstanceUnique();
    uint32_t sum = DataView(reinterpret_cast<char*>(instance.bytes)).read<uint32_t>(1) +
        (durationCount<Seconds>(time.toDurationSinceEpoch()) - roundedSeconds);
    DataView(reinterpret_cast<char*>(instance.bytes)).write<uint32_t>(sum, 1);
    oid.setInstanceUnique(instance);

    return {oid, roundedTime};
}

Bucket& allocateBucket(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const CreationInfo& info) {
    expireIdleBuckets(catalog, stripe, stripeLock, info.stats, *info.closedBuckets);

    // In rare cases duplicate bucket _id fields can be generated in the same stripe and fail to be
    // inserted. We will perform a limited number of retries to minimize the probability of
    // collision.
    auto maxRetries = gTimeseriesInsertMaxRetriesOnDuplicates.load();
    OID oid;
    Date_t roundedTime;
    stdx::unordered_map<BucketId, std::unique_ptr<Bucket>, BucketHasher>::iterator it;
    bool inserted = false;
    for (int retryAttempts = 0; !inserted && retryAttempts < maxRetries; ++retryAttempts) {
        std::tie(oid, roundedTime) = generateBucketOID(info.time, info.options);
        auto bucketId = BucketId{info.key.ns, oid};
        std::tie(it, inserted) = stripe.openBucketsById.try_emplace(
            bucketId,
            std::make_unique<Bucket>(bucketId,
                                     info.key,
                                     info.options.getTimeField(),
                                     roundedTime,
                                     catalog.bucketStateRegistry));
    }
    uassert(6130900,
            "Unable to insert documents due to internal OID generation collision. Increase the "
            "value of server parameter 'timeseriesInsertMaxRetriesOnDuplicates' and try again",
            inserted);

    Bucket* bucket = it->second.get();
    stripe.openBucketsByKey[info.key].emplace(bucket);

    auto state = changeBucketState(
        catalog.bucketStateRegistry,
        it->first,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(!input.has_value());
            return BucketState{};
        });
    invariant(state == BucketState{});
    catalog.numberOfActiveBuckets.fetchAndAdd(1);

    if (info.openedDuetoMetadata) {
        info.stats.incNumBucketsOpenedDueToMetadata();
    }

    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(info.options.getTimeField(), roundedTime);
    bucket->minmax.update(
        controlDoc, /*metaField=*/boost::none, bucket->key.metadata.getComparator());
    return *bucket;
}

Bucket& rollover(BucketCatalog& catalog,
                 Stripe& stripe,
                 WithLock stripeLock,
                 Bucket& bucket,
                 const CreationInfo& info,
                 RolloverAction action) {
    invariant(action != RolloverAction::kNone);
    if (allCommitted(bucket)) {
        // The bucket does not contain any measurements that are yet to be committed, so we can take
        // action now.
        if (action == RolloverAction::kArchive) {
            archiveBucket(catalog, stripe, stripeLock, bucket, *info.closedBuckets);
        } else {
            closeOpenBucket(catalog, stripe, stripeLock, bucket, *info.closedBuckets);
        }
    } else {
        // We must keep the bucket around until all measurements are committed committed, just mark
        // the action we chose now so it we know what to do when the last batch finishes.
        bucket.rolloverAction = action;
    }

    return allocateBucket(catalog, stripe, stripeLock, info);
}

std::pair<RolloverAction, RolloverReason> determineRolloverAction(
    OperationContext* opCtx,
    const BSONObj& doc,
    CreationInfo& info,
    Bucket& bucket,
    uint32_t numberOfActiveBuckets,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    int32_t& sizeToBeAdded,
    AllowBucketCreation mode) {
    // If the mode is enabled to create new buckets, then we should update stats for soft closures
    // accordingly. If we specify the mode to not allow bucket creation, it means we are not sure if
    // we want to soft close the bucket yet and should wait to update closure stats.
    const bool shouldUpdateStats = (mode == AllowBucketCreation::kYes);

    auto bucketTime = bucket.minTime;
    if (info.time - bucketTime >= Seconds(*info.options.getBucketMaxSpanSeconds())) {
        if (shouldUpdateStats) {
            info.stats.incNumBucketsClosedDueToTimeForward();
        }
        return {RolloverAction::kSoftClose, RolloverReason::kTimeForward};
    }
    if (info.time < bucketTime) {
        const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility);
        if (shouldUpdateStats) {
            if (canArchive) {
                info.stats.incNumBucketsArchivedDueToTimeBackward();
            } else {
                info.stats.incNumBucketsClosedDueToTimeBackward();
            }
        }
        return {canArchive ? RolloverAction::kArchive : RolloverAction::kSoftClose,
                RolloverReason::kTimeBackward};
    }
    if (bucket.numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
        info.stats.incNumBucketsClosedDueToCount();
        return {RolloverAction::kHardClose, RolloverReason::kCount};
    }

    // In scenarios where we have a high cardinality workload and face increased cache pressure we
    // will decrease the size of buckets before we close them.
    int32_t cacheDerivedBucketMaxSize = getCacheDerivedBucketMaxSize(
        opCtx->getServiceContext()->getStorageEngine(), numberOfActiveBuckets);
    int32_t effectiveMaxSize = std::min(gTimeseriesBucketMaxSize, cacheDerivedBucketMaxSize);

    // Before we hit our bucket minimum count, we will allow for large measurements to be inserted
    // into buckets. Instead of packing the bucket to the BSON size limit, 16MB, we'll limit the max
    // bucket size to 12MB. This is to leave some space in the bucket if we need to add new internal
    // fields to existing, full buckets.
    static constexpr int32_t largeMeasurementsMaxBucketSize =
        BSONObjMaxUserSize - (4 * 1024 * 1024);
    // We restrict the ceiling of the bucket max size under cache pressure.
    int32_t absoluteMaxSize = std::min(largeMeasurementsMaxBucketSize, cacheDerivedBucketMaxSize);

    calculateBucketFieldsAndSizeChange(
        bucket, doc, info.options.getMetaField(), newFieldNamesToBeInserted, sizeToBeAdded);
    if (bucket.size + sizeToBeAdded > effectiveMaxSize) {
        bool keepBucketOpenForLargeMeasurements =
            bucket.numMeasurements < static_cast<std::uint64_t>(gTimeseriesBucketMinCount) &&
            feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                serverGlobalParams.featureCompatibility);
        if (keepBucketOpenForLargeMeasurements) {
            if (bucket.size + sizeToBeAdded > absoluteMaxSize) {
                if (absoluteMaxSize != largeMeasurementsMaxBucketSize) {
                    info.stats.incNumBucketsClosedDueToCachePressure();
                    return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
                }
                info.stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }

            // There's enough space to add this measurement and we're still below the large
            // measurement threshold.
            if (!bucket.keptOpenDueToLargeMeasurements) {
                // Only increment this metric once per bucket.
                bucket.keptOpenDueToLargeMeasurements = true;
                info.stats.incNumBucketsKeptOpenDueToLargeMeasurements();
            }
            return {RolloverAction::kNone, RolloverReason::kNone};
        } else {
            if (effectiveMaxSize == gTimeseriesBucketMaxSize) {
                info.stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }
            info.stats.incNumBucketsClosedDueToCachePressure();
            return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
        }
    }

    if (schemaIncompatible(
            bucket, doc, info.options.getMetaField(), info.key.metadata.getComparator())) {
        info.stats.incNumBucketsClosedDueToSchemaChange();
        return {RolloverAction::kHardClose, RolloverReason::kSchemaChange};
    }

    return {RolloverAction::kNone, RolloverReason::kNone};
}

ExecutionStatsController getOrInitializeExecutionStats(BucketCatalog& catalog,
                                                       const NamespaceString& ns) {
    stdx::lock_guard catalogLock{catalog.mutex};
    auto it = catalog.executionStats.find(ns);
    if (it != catalog.executionStats.end()) {
        return {it->second, catalog.globalExecutionStats};
    }

    auto res = catalog.executionStats.emplace(ns, std::make_shared<ExecutionStats>());
    return {res.first->second, catalog.globalExecutionStats};
}

std::shared_ptr<ExecutionStats> getExecutionStats(const BucketCatalog& catalog,
                                                  const NamespaceString& ns) {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};

    stdx::lock_guard catalogLock{catalog.mutex};

    auto it = catalog.executionStats.find(ns);
    if (it != catalog.executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

Status getTimeseriesBucketClearedError(const NamespaceString& ns, const OID& oid) {
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << oid << " for namespace "
                          << ns.toStringForErrorMsg() << " was cleared"};
}

void closeOpenBucket(BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     ClosedBuckets& closedBuckets) {
    bool error = false;
    try {
        closedBuckets.emplace_back(&catalog.bucketStateRegistry,
                                   bucket.bucketId,
                                   bucket.timeField,
                                   bucket.numMeasurements);
    } catch (...) {
        error = true;
    }
    removeBucket(
        catalog, stripe, stripeLock, bucket, error ? RemovalMode::kAbort : RemovalMode::kClose);
}

void closeOpenBucket(BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     boost::optional<ClosedBucket>& closedBucket) {
    bool error = false;
    try {
        closedBucket = boost::in_place(&catalog.bucketStateRegistry,
                                       bucket.bucketId,
                                       bucket.timeField,
                                       bucket.numMeasurements);
    } catch (...) {
        closedBucket = boost::none;
        error = true;
    }
    removeBucket(
        catalog, stripe, stripeLock, bucket, error ? RemovalMode::kAbort : RemovalMode::kClose);
}

void closeArchivedBucket(BucketStateRegistry& registry,
                         ArchivedBucket& bucket,
                         ClosedBuckets& closedBuckets) {
    try {
        closedBuckets.emplace_back(&registry, bucket.bucketId, bucket.timeField, boost::none);
    } catch (...) {
    }
}

}  // namespace mongo::timeseries::bucket_catalog::internal
