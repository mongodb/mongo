/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/timeseries_modify.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/matcher/match_details.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/update_util.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/decorable.h"

#include <string>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo {
const char* TimeseriesModifyStage::kStageType = "TS_MODIFY";

TimeseriesModifyStage::TimeseriesModifyStage(ExpressionContext* expCtx,
                                             TimeseriesModifyParams&& params,
                                             WorkingSet* ws,
                                             std::unique_ptr<PlanStage> child,
                                             CollectionAcquisition coll,
                                             timeseries::BucketUnpacker bucketUnpacker,
                                             std::unique_ptr<MatchExpression> residualPredicate,
                                             std::unique_ptr<MatchExpression> originalPredicate)
    : RequiresWritableCollectionStage(kStageType, expCtx, coll),
      _params(std::move(params)),
      _originalPredicate(std::move(originalPredicate)),
      _ws(ws),
      _bucketUnpacker{std::move(bucketUnpacker)},
      _residualPredicate(std::move(residualPredicate)),
      _preWriteFilter(opCtx(), coll.nss()) {
    tassert(7308200,
            "Multi deletes must have a residual predicate",
            _isSingletonWrite() || _residualPredicate || _params.isUpdate);
    tassert(7308300,
            "Can return the old measurement only if modifying one",
            !_params.returnOld || _isSingletonWrite());
    tassert(7314602,
            "Can return the new measurement only if updating one",
            !_params.returnNew || (_isSingletonWrite() && _params.isUpdate));
    tassert(7743100,
            "Updates must provide original predicate",
            !_params.isUpdate || _originalPredicate);
    _children.emplace_back(std::move(child));

    // These three properties are only used for the queryPlanner explain and will not change while
    // executing this stage.
    _specificStats.opType = [&] {
        if (_params.isUpdate) {
            return _isMultiWrite() ? "updateMany" : "updateOne";
        }
        return _isMultiWrite() ? "deleteMany" : "deleteOne";
    }();
    _specificStats.bucketFilter = _params.canonicalQuery->getQueryObj();
    if (_residualPredicate) {
        _specificStats.residualFilter = _residualPredicate->serialize();
    }

    tassert(7314202,
            "Updates must specify an update driver",
            _params.updateDriver || !_params.isUpdate);
    _specificStats.isModUpdate =
        _params.isUpdate && _params.updateDriver->type() == UpdateDriver::UpdateType::kOperator;

    _isUserInitiatedUpdate = _params.isUpdate && opCtx()->writesAreReplicated() &&
        !(_params.isFromOplogApplication ||
          _params.updateDriver->type() == UpdateDriver::UpdateType::kDelta || _params.fromMigrate);

    if (_params.isUpdate) {
        _sideBucketCatalog = std::make_unique<timeseries::bucket_catalog::BucketCatalog>(
            1, getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes);
        timeseriesCounters.incrementMeasurementUpdate();
    } else {
        // The command is performing a time-series measurement delete.
        timeseriesCounters.incrementMeasurementDelete();
    }
}

TimeseriesModifyStage::~TimeseriesModifyStage() {
    if (_sideBucketCatalog && !_insertedBucketIds.empty()) {
        auto [collectionUUID, collStats] =
            timeseries::bucket_catalog::getSideBucketCatalogCollectionStats(*_sideBucketCatalog);
        // Finishes tracking the newly inserted buckets in the main bucket catalog as direct
        // writes when the whole update operation is done.
        auto& bucketCatalog =
            timeseries::bucket_catalog::GlobalBucketCatalog::get(opCtx()->getServiceContext());
        for (const auto bucketId : _insertedBucketIds) {
            timeseries::bucket_catalog::directWriteFinish(bucketCatalog.bucketStateRegistry,
                                                          bucketId);
        }
        // Merges the execution stats of the side bucket catalog to the main one.
        timeseries::bucket_catalog::mergeExecutionStatsToBucketCatalog(
            bucketCatalog, collStats, collectionUUID);
    }
}

bool TimeseriesModifyStage::isEOF() const {
    if (_isSingletonWrite() && _specificStats.nMeasurementsMatched > 0) {
        // If we have matched any records and this is a singleton write, we can return as long as we
        // don't have a bucket to retry.
        return _retryBucketId == WorkingSet::INVALID_ID;
    }
    return child()->isEOF() && _retryBucketId == WorkingSet::INVALID_ID;
}

std::unique_ptr<PlanStageStats> TimeseriesModifyStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<TimeseriesModifyStats>(_specificStats);
    for (const auto& child : _children) {
        ret->children.emplace_back(child->getStats());
    }
    return ret;
}

const std::vector<std::unique_ptr<FieldRef>>& TimeseriesModifyStage::_getUserLevelShardKeyPaths(
    const ScopedCollectionDescription& collDesc) {
    _immutablePaths.clear();

    const auto& tsFields = collDesc.getTimeseriesFields();
    for (const auto& shardKeyField : collDesc.getKeyPatternFields()) {
        if (auto metaField = tsFields->getMetaField(); metaField &&
            shardKeyField->isPrefixOfOrEqualTo(FieldRef{timeseries::kBucketMetaFieldName})) {
            auto userMetaFieldRef = std::make_unique<FieldRef>(*metaField);
            if (shardKeyField->numParts() > 1) {
                userMetaFieldRef->appendPart(shardKeyField->dottedField(1));
            }
            _immutablePaths.emplace_back(std::move(userMetaFieldRef));
        } else if (auto timeField = tsFields->getTimeField();
                   shardKeyField->isPrefixOfOrEqualTo(
                       FieldRef{timeseries::kControlMinFieldNamePrefix + std::string{timeField}}) ||
                   shardKeyField->isPrefixOfOrEqualTo(
                       FieldRef{timeseries::kControlMaxFieldNamePrefix + std::string{timeField}})) {
            _immutablePaths.emplace_back(std::make_unique<FieldRef>(timeField));
        } else {
            tasserted(7687100,
                      fmt::format("Unexpected shard key field: {}", shardKeyField->dottedField()));
        }
    }

    return _immutablePaths;
}

const std::vector<std::unique_ptr<FieldRef>>& TimeseriesModifyStage::_getImmutablePaths() {
    if (!_isUserInitiatedUpdate) {
        return _immutablePaths;
    }

    const auto& collDesc = collectionAcquisition().getShardingDescription();
    if (!collDesc.isSharded() || OperationShardingState::isComingFromRouter(opCtx())) {
        return _immutablePaths;
    }

    return _getUserLevelShardKeyPaths(collDesc);
}

std::vector<BSONObj> TimeseriesModifyStage::_applyUpdate(
    const std::vector<BSONObj>& matchedMeasurements, std::vector<BSONObj>& unchangedMeasurements) {
    // Determine which documents to update based on which ones are actually being changed.
    std::vector<BSONObj> modifiedMeasurements;

    for (auto&& measurement : matchedMeasurements) {
        // Timeseries updates are never in place, because we execute them as a delete of the old
        // measurement plus an insert of the modified one.
        mutablebson::Document doc(measurement, mutablebson::Document::kInPlaceDisabled);

        // We want to block shard key updates if the user requested an update directly to a shard,
        // when shard key fields should be immutable.
        FieldRefSet immutablePaths(_getImmutablePaths());
        const bool isInsert = false;
        bool docWasModified = false;

        if (!_params.updateDriver->needMatchDetails()) {
            uassertStatusOK(_params.updateDriver->update(opCtx(),
                                                         "",
                                                         &doc,
                                                         _isUserInitiatedUpdate,
                                                         immutablePaths,
                                                         isInsert,
                                                         nullptr,
                                                         &docWasModified));
        } else {
            // If there was a matched field, obtain it.
            MatchDetails matchDetails;
            matchDetails.requestElemMatchKey();

            // We have to re-apply the filter to get the matched element.
            tassert(
                7662500,
                "measurement must pass filter",
                exec::matcher::matchesBSON(_originalPredicate.get(), measurement, &matchDetails));

            uassertStatusOK(_params.updateDriver->update(
                opCtx(),
                matchDetails.hasElemMatchKey() ? matchDetails.elemMatchKey() : "",
                &doc,
                _isUserInitiatedUpdate,
                immutablePaths,
                isInsert,
                nullptr,
                &docWasModified));
        }

        if (docWasModified) {
            modifiedMeasurements.emplace_back(doc.getObject());
        } else {
            // The document wasn't modified, write it back to the original bucket unchanged.
            unchangedMeasurements.emplace_back(measurement);
        }
    }

    return modifiedMeasurements;
}

void TimeseriesModifyStage::_checkRestrictionsOnUpdatingShardKeyAreNotViolated(
    const ScopedCollectionDescription& collDesc, const FieldRefSet& shardKeyPaths) {
    // We do not allow updates to the shard key when 'multi' is true.
    uassert(ErrorCodes::InvalidOptions,
            "Multi-update operations are not allowed when updating the shard key field.",
            _params.isUpdate && _isSingletonWrite());

    // $_allowShardKeyUpdatesWithoutFullShardKeyInQuery is an internal parameter, used exclusively
    // by the two-phase write protocol for updateOne without shard key.
    if (_params.allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
        bool isInternalThreadOrClient = !cc().session() || cc().isInternalClient();
        uassert(ErrorCodes::InvalidOptions,
                "$_allowShardKeyUpdatesWithoutFullShardKeyInQuery is an internal parameter",
                isInternalThreadOrClient);
    }

    // If this node is a replica set primary node, an attempted update to the shard key value must
    // either be a retryable write or inside a transaction. An update without a transaction number
    // is legal if gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi is enabled because mongos
    // will be able to start an internal transaction to handle the wouldChangeOwningShard error
    // thrown below. If this node is a replica set secondary node, we can skip validation.
    if (!feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
            VersionContext::getDecoration(opCtx()),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // As of v7.1, MongoDB supports WCOS updates without having a full shard key in the filter,
        // provided the update is retryable or in transaction. Normally the shards can enforce this
        // rule by simply checking 'opCtx'.  However, for some write types, the router creates an
        // internal transaction and runs commands on the shards inside this transaction. To ensure
        // WCOS updates are only allowed if the original command was retryable or transaction, the
        // router will explicitly set $_allowShardKeyUpdatesWithoutFullShardKeyInQuery to true or
        // false to instruct whether WCOS updates should be allowed.
        uassert(ErrorCodes::IllegalOperation,
                "Must run update to shard key field in a multi-statement transaction or with "
                "retryWrites: true.",
                _params.allowShardKeyUpdatesWithoutFullShardKeyInQuery.value_or(
                    static_cast<bool>(opCtx()->getTxnNumber())));
    }
}

void TimeseriesModifyStage::_checkUpdateChangesExistingShardKey(const BSONObj& newBucket,
                                                                const BSONObj& oldBucket,
                                                                const BSONObj& newMeasurement,
                                                                const BSONObj& oldMeasurement) {
    const auto& collDesc = collectionAcquisition().getShardingDescription();
    const auto& shardKeyPattern = collDesc.getShardKeyPattern();

    auto oldShardKey = shardKeyPattern.extractShardKeyFromDoc(oldBucket);
    auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newBucket);

    // If the shard key fields remain unchanged by this update we can skip the rest of the checks.
    // Using BSONObj::binaryEqual() still allows a missing shard key field to be filled in with an
    // explicit null value.
    if (newShardKey.binaryEqual(oldShardKey)) {
        return;
    }

    FieldRefSet shardKeyPaths(collDesc.getKeyPatternFields());

    // Assert that the updated doc has no arrays or array descendants for the shard key fields.
    update::assertPathsNotArray(mutablebson::Document{oldBucket}, shardKeyPaths);

    _checkRestrictionsOnUpdatingShardKeyAreNotViolated(collDesc, shardKeyPaths);

    // At this point we already asserted that the complete shardKey have been specified in the
    // query, this implies that mongos is not doing a broadcast update and that it attached a
    // shardVersion to the command. Thus it is safe to call getOwnershipFilter
    const auto& collFilter = collectionAcquisition().getShardingFilter();
    invariant(collFilter);

    // If the shard key of an orphan document is allowed to change, and the document is allowed to
    // become owned by the shard, the global uniqueness assumption for _id values would be violated.
    invariant(collFilter->keyBelongsToMe(oldShardKey));

    if (!collFilter->keyBelongsToMe(newShardKey)) {
        // We send the 'oldMeasurement' instead of the old bucket document to leverage timeseries
        // deleteOne because the delete can run inside an internal transaction.
        uasserted(WouldChangeOwningShardInfo(oldMeasurement,
                                             newBucket,
                                             false,
                                             collectionPtr()->ns(),
                                             collectionPtr()->uuid(),
                                             newMeasurement),
                  "This update would cause the doc to change owning shards");
    }
}

void TimeseriesModifyStage::_checkUpdateChangesReshardingKey(
    const ShardingWriteRouter& shardingWriteRouter,
    const BSONObj& newBucket,
    const BSONObj& oldBucket,
    const BSONObj& newMeasurement,
    const BSONObj& oldMeasurement) {
    const auto& collDesc = collectionAcquisition().getShardingDescription();

    auto reshardingKeyPattern = collDesc.getReshardingKeyIfShouldForwardOps();
    if (!reshardingKeyPattern)
        return;

    auto oldShardKey = reshardingKeyPattern->extractShardKeyFromDoc(oldBucket);
    auto newShardKey = reshardingKeyPattern->extractShardKeyFromDoc(newBucket);

    if (newShardKey.binaryEqual(oldShardKey))
        return;

    FieldRefSet shardKeyPaths(collDesc.getKeyPatternFields());
    _checkRestrictionsOnUpdatingShardKeyAreNotViolated(collDesc, shardKeyPaths);

    auto oldRecipShard = *shardingWriteRouter.getReshardingDestinedRecipient(oldBucket);
    auto newRecipShard = *shardingWriteRouter.getReshardingDestinedRecipient(newBucket);

    if (oldRecipShard != newRecipShard) {
        // We send the 'oldMeasurement' instead of the old bucket document to leverage timeseries
        // deleteOne because the delete can run inside an internal transaction.
        uasserted(
            WouldChangeOwningShardInfo(oldMeasurement,
                                       newBucket,
                                       false,
                                       collectionPtr()->ns(),
                                       collectionPtr()->uuid(),
                                       newMeasurement),
            "This update would cause the doc to change owning shards under the new shard key");
    }
}

void TimeseriesModifyStage::_checkUpdateChangesShardKeyFields(const BSONObj& newBucket,
                                                              const BSONObj& oldBucket,
                                                              const BSONObj& newMeasurement,
                                                              const BSONObj& oldMeasurement) {
    const auto isSharded = collectionAcquisition().getShardingDescription().isSharded();
    if (!isSharded) {
        return;
    }

    // It is possible that both the existing and new shard keys are being updated, so we do not want
    // to short-circuit checking whether either is being modified.
    _checkUpdateChangesExistingShardKey(newBucket, oldBucket, newMeasurement, oldMeasurement);
    ShardingWriteRouter shardingWriteRouter(opCtx(), collectionPtr()->ns());
    _checkUpdateChangesReshardingKey(
        shardingWriteRouter, newBucket, oldBucket, newMeasurement, oldMeasurement);
}

template <typename F>
std::tuple<bool, PlanStage::StageState, boost::optional<BSONObj>>
TimeseriesModifyStage::_writeToTimeseriesBuckets(ScopeGuard<F>& bucketFreer,
                                                 WorkingSetID bucketWsmId,
                                                 std::vector<BSONObj>&& unchangedMeasurements,
                                                 std::vector<BSONObj>&& matchedMeasurements,
                                                 Date_t currentMinTime,
                                                 bool bucketFromMigrate) {
    // No measurements needed to be updated or deleted from the bucket document.
    if (matchedMeasurements.empty()) {
        return {false, PlanStage::NEED_TIME, boost::none};
    }

    bool isUpdate = _params.isUpdate;

    // If this is a delete, we will be deleting all matched measurements. If this is an update, we
    // may not need to modify all measurements, since some may be no-op updates.
    const auto& modifiedMeasurements =
        isUpdate ? _applyUpdate(matchedMeasurements, unchangedMeasurements) : matchedMeasurements;

    // Checks for shard key value changes. We will fail the command if it's a multi-update, so only
    // performing the check needed for a single-update.
    if (isUpdate && _isUserInitiatedUpdate && !modifiedMeasurements.empty()) {
        _checkUpdateChangesShardKeyFields(
            timeseries::write_ops::makeBucketDocument({modifiedMeasurements[0]},
                                                      collectionPtr()->ns(),
                                                      collectionPtr()->uuid(),
                                                      *collectionPtr()->getTimeseriesOptions(),
                                                      collectionPtr()->getDefaultCollator()),
            _bucketUnpacker.bucket(),
            modifiedMeasurements[0],
            matchedMeasurements[0]);
    }

    auto getMeasurementToReturn = [&] {
        // If asked to return the old or new measurement and the write was successful, we should
        // save the measurement so that we can return it later.
        if (_params.returnOld) {
            return boost::optional<BSONObj>(std::move(matchedMeasurements[0]));
        } else if (_params.returnNew) {
            if (modifiedMeasurements.empty()) {
                // If we are returning the new measurement, then we must have modified at least one
                // measurement. If we did not, then we should return the old measurement instead.
                return boost::optional<BSONObj>(std::move(matchedMeasurements[0]));
            } else {
                return boost::optional<BSONObj>(modifiedMeasurements[0]);
            }
        }
        return boost::optional<BSONObj>();
    };

    // After applying the updates, no measurements needed to be updated in the bucket document. This
    // case is still considered a successful write.
    if (modifiedMeasurements.empty()) {
        return {true, PlanStage::NEED_TIME, getMeasurementToReturn()};
    }

    // We don't actually write anything if we are in explain mode but we still need to update the
    // stats and let the caller think as if the write succeeded if there's any modified measurement.
    if (_params.isExplain) {
        _specificStats.nMeasurementsModified += modifiedMeasurements.size();
        return {true, PlanStage::NEED_TIME, getMeasurementToReturn()};
    }

    const auto saveRet = handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage saveState",
        [&] {
            child()->saveState();
            return PlanStage::NEED_TIME;
        },
        [&] {
            // yieldHandler
            // We need to retry the bucket, so we should not free the current bucket.
            bucketFreer.dismiss();
            _retryBucket(bucketWsmId);
        });
    if (saveRet != PlanStage::NEED_TIME) {
        return {false, saveRet, boost::none};
    }

    auto recordId = _ws->get(bucketWsmId)->recordId;
    try {
        const auto modificationRet = handlePlanStageYield(
            expCtx(),
            "TimeseriesModifyStage writeToBuckets",
            [&] {
                if (isUpdate) {
                    timeseries::performAtomicWritesForUpdate(opCtx(),
                                                             collectionPtr(),
                                                             recordId,
                                                             unchangedMeasurements,
                                                             modifiedMeasurements,
                                                             *_sideBucketCatalog,
                                                             bucketFromMigrate,
                                                             _params.stmtId,
                                                             &_insertedBucketIds,
                                                             currentMinTime);
                } else {
                    timeseries::performAtomicWritesForDelete(opCtx(),
                                                             collectionPtr(),
                                                             recordId,
                                                             unchangedMeasurements,
                                                             bucketFromMigrate,
                                                             _params.stmtId,
                                                             currentMinTime);
                }
                return PlanStage::NEED_TIME;
            },
            [&] {
                // yieldHandler
                // We need to retry the bucket, so we should not free the current bucket.
                bucketFreer.dismiss();
                _retryBucket(bucketWsmId);
            });
        if (modificationRet != PlanStage::NEED_TIME) {
            return {false, PlanStage::NEED_YIELD, boost::none};
        }
    } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
        if (ShardVersion::isPlacementVersionIgnored(ex->getVersionReceived()) &&
            ex->getCriticalSectionSignal()) {
            // If the placement version is IGNORED and we encountered a critical section, then
            // yield, wait for the critical section to finish and then we'll resume the write
            // from the point we had left. We do this to prevent large multi-writes from
            // repeatedly failing due to StaleConfig and exhausting the mongos retry attempts.
            planExecutorShardingState(opCtx()).criticalSectionFuture =
                ex->getCriticalSectionSignal();
            // We need to retry the bucket, so we should not free the current bucket.
            bucketFreer.dismiss();
            _retryBucket(bucketWsmId);
            return {false, PlanStage::NEED_YIELD, boost::none};
        }
        throw;
    }
    _specificStats.nMeasurementsModified += modifiedMeasurements.size();

    // As restoreState may restore (recreate) cursors, cursors are tied to the transaction in which
    // they are created, and a WriteUnitOfWork is a transaction, make sure to restore the state
    // outside of the WriteUnitOfWork.
    auto status = handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage restoreState",
        [&] {
            child()->restoreState(&collectionPtr());
            return PlanStage::NEED_TIME;
        },
        // yieldHandler
        // Note we don't need to retry anything in this case since the write already was committed.
        // However, we still need to return the affected measurement (if it was requested). We don't
        // need to rely on the storage engine to return the affected document since we already have
        // it in memory.
        [&] { /* noop */ });

    if (status == NEED_YIELD && isEOF() &&
        !shard_role_details::getLocker(opCtx())->inAWriteUnitOfWork()) {
        // If this stage is already exhausted it won't use its children stages anymore and therefore
        // it's okay if we failed to restore them. Avoid requesting a yield to the plan executor.
        // Restoring from yield could fail due to a sharding placement change. Throwing a
        // StaleConfig error is undesirable after an "update one" operation has already performed a
        // write because the router would retry.
        //
        // If this plan is part of a larger encompassing WUOW it would be illegal to skip returning
        // NEED_YIELD, so we don't skip it. In this case, such as multi-doc transactions, this is
        // okay as the PlanExecutor is not allowed to auto-yield.
        status = PlanStage::NEED_TIME;
    }

    return {true, status, getMeasurementToReturn()};
}

template <typename F>
std::pair<boost::optional<PlanStage::StageState>, bool>
TimeseriesModifyStage::_checkIfWritingToOrphanedBucket(ScopeGuard<F>& bucketFreer,
                                                       WorkingSetID id) {
    // If we are in explain mode, we do not need to check if the bucket is orphaned since we're not
    // writing to bucket. If we are migrating a bucket, we also do not need to check if the bucket
    // is not writable and just return it.
    if (_params.isExplain || _params.fromMigrate) {
        return {boost::none, _params.fromMigrate};
    }
    return _preWriteFilter.checkIfNotWritable(
        _ws->get(id)->doc.value(),
        "timeseries "_sd + _specificStats.opType,
        collectionPtr()->ns(),
        [&](const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
            planExecutorShardingState(opCtx()).criticalSectionFuture =
                ex->getCriticalSectionSignal();
            // Retry the write if we're in the sharding
            // critical section.
            bucketFreer.dismiss();
            _retryBucket(id);
        });
}

PlanStage::StageState TimeseriesModifyStage::_getNextBucket(WorkingSetID& id) {
    if (_retryBucketId == WorkingSet::INVALID_ID) {
        auto status = child()->work(&id);
        if (status != PlanStage::ADVANCED) {
            return status;
        }
    } else {
        id = _retryBucketId;
        _retryBucketId = WorkingSet::INVALID_ID;
    }

    // We may not have an up-to-date bucket for this RecordId. Fetch it and ensure that it still
    // exists and matches our bucket-level predicate if it is not believed to be up-to-date.
    bool docStillMatches;

    const auto status = handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage:: ensureStillMatches",
        [&] {
            docStillMatches = write_stage_common::ensureStillMatches(
                collectionPtr(), opCtx(), _ws, id, _params.canonicalQuery);
            return PlanStage::NEED_TIME;
        },
        [&] {
            // yieldHandler
            // There was a problem trying to detect if the document still exists, so retry.
            _retryBucket(id);
        });
    if (status != PlanStage::NEED_TIME) {
        return status;
    }
    return docStillMatches ? PlanStage::ADVANCED : PlanStage::NEED_TIME;
}

void TimeseriesModifyStage::_retryBucket(WorkingSetID bucketId) {
    tassert(7309302,
            "Cannot be in the middle of unpacking a bucket if retrying",
            !_bucketUnpacker.hasNext());
    tassert(7309303,
            "Cannot retry two buckets at the same time",
            _retryBucketId == WorkingSet::INVALID_ID);

    _retryBucketId = bucketId;
}

void TimeseriesModifyStage::_prepareToReturnMeasurement(WorkingSetID& out, BSONObj measurement) {
    tassert(7314601,
            "Must be called only when need to return the old or new measurement",
            _params.returnOld || _params.returnNew);

    out = _ws->allocate();
    auto member = _ws->get(out);
    // The measurement does not have record id.
    member->recordId = RecordId{};
    member->doc.value() = Document{measurement};
    _ws->transitionToOwnedObj(out);
}

PlanStage::StageState TimeseriesModifyStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    tassert(7495500,
            "Expected bucketUnpacker's current bucket to be exhausted",
            !_bucketUnpacker.hasNext());

    auto id = WorkingSet::INVALID_ID;
    auto status = _getNextBucket(id);
    if (status != PlanStage::ADVANCED) {
        if (status == PlanStage::NEED_YIELD) {
            *out = id;
        }
        return status;
    }

    // We want to free this member when we return because we either have an owned copy of the bucket
    // for normal write and write to orphan cases, or we skip the bucket.
    ScopeGuard bucketFreer([&] { _ws->free(id); });

    auto member = _ws->get(id);
    tassert(7459100, "Expected a RecordId from the child stage", member->hasRecordId());

    // Determine if we are writing to an orphaned bucket - such writes should be excluded from
    // user-visible change stream events. This will be achieved later by setting 'fromMigrate' flag
    // when calling performAtomicWrites().
    auto [immediateReturnStageState, bucketFromMigrate] =
        _checkIfWritingToOrphanedBucket(bucketFreer, id);
    if (immediateReturnStageState) {
        return *immediateReturnStageState;
    }
    tassert(7309304,
            "Expected no bucket to retry after getting a new bucket",
            _retryBucketId == WorkingSet::INVALID_ID);

    // Unpack the bucket and determine which measurements match the residual predicate.
    auto ownedBucket = member->doc.value().toBson().getOwned();
    // Get the minTime of the bucket we are unpacking. We rely on the invariant that the minTime of
    // the bucket will not be changed, even if it is updated or has some of its measurements
    // deleted.
    auto timeseriesOptions = collectionPtr()->getTimeseriesOptions();
    Date_t currentMinTime = ownedBucket.getObjectField(timeseries::kBucketControlFieldName)
                                .getObjectField(timeseries::kBucketControlMinFieldName)
                                .getField(timeseriesOptions->getTimeField())
                                .Date();
    _bucketUnpacker.reset(std::move(ownedBucket));
    // Closed buckets should have been filtered out by the bucket predicate.
    tassert(7554700, "Expected bucket to not be closed", !_bucketUnpacker.isClosedBucket());
    ++_specificStats.nBucketsUnpacked;

    std::vector<BSONObj> unchangedMeasurements;
    std::vector<BSONObj> matchedMeasurements;

    while (_bucketUnpacker.hasNext()) {
        auto measurement = _bucketUnpacker.getNext().toBson();
        // We should stop matching measurements once we hit the limit of one in the non-multi case.
        bool shouldContinueMatching = _isMultiWrite() || matchedMeasurements.empty();
        if (shouldContinueMatching &&
            (!_residualPredicate ||
             exec::matcher::matchesBSON(_residualPredicate.get(), measurement))) {
            matchedMeasurements.push_back(measurement);
        } else {
            unchangedMeasurements.push_back(measurement);
        }
    }

    auto isWriteSuccessful = false;
    boost::optional<BSONObj> measurementToReturn = boost::none;
    auto numMatchedMeasurements = matchedMeasurements.size();
    std::tie(isWriteSuccessful, status, measurementToReturn) =
        _writeToTimeseriesBuckets(bucketFreer,
                                  id,
                                  std::move(unchangedMeasurements),
                                  std::move(matchedMeasurements),
                                  currentMinTime,
                                  bucketFromMigrate);
    if (status != PlanStage::NEED_TIME) {
        *out = WorkingSet::INVALID_ID;
    } else if (isWriteSuccessful) {
        _specificStats.nMeasurementsMatched += numMatchedMeasurements;
        if (measurementToReturn) {
            // If the write was successful and if asked to return the old or new measurement, then
            // 'measurementToReturn' must have been filled out and we can return it immediately.
            _prepareToReturnMeasurement(*out, std::move(*measurementToReturn));
            status = PlanStage::ADVANCED;
        }
    }

    if (status == PlanStage::NEED_TIME && isEOF()) {
        status = PlanStage::IS_EOF;
    }

    return status;
}

void TimeseriesModifyStage::doRestoreStateRequiresCollection() {
    const NamespaceString& ns = collectionPtr()->ns();
    uassert(ErrorCodes::PrimarySteppedDown,
            fmt::format("Demoted from primary while removing from {}", ns.toStringForErrorMsg()),
            !opCtx()->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx())->canAcceptWritesFor(opCtx(), ns));

    const bool isSingleWriteAndAlreadyWrote = !_params.isMulti &&
        (_specificStats.nMeasurementsModified || _specificStats.nMeasurementsUpserted > 0);
    tassert(7940800,
            "Single write should never restore after having already modified one document.",
            !isSingleWriteAndAlreadyWrote || _params.isExplain);

    _preWriteFilter.restoreState();
}
}  // namespace mongo
