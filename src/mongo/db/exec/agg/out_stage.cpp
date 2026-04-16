/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/out_stage.h"

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/writer_util.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceOutToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto ds = boost::dynamic_pointer_cast<DocumentSourceOut>(documentSource);

    tassert(10561501, "expected 'DocumentSourceOut' type", ds);

    return make_intrusive<exec::agg::OutStage>(
        ds->kStageName, ds->getExpCtx(), ds->getOutputNs(), ds->_timeseries, ds->getMergeShardId());
}

namespace exec {
namespace agg {

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceOutBatch);
MONGO_FAIL_POINT_DEFINE(outWaitAfterTempCollectionCreation);
MONGO_FAIL_POINT_DEFINE(outWaitBeforeTempCollectionRename);
MONGO_FAIL_POINT_DEFINE(outWaitAfterTempCollectionRenameBeforeView);
MONGO_FAIL_POINT_DEFINE(outImplictlyCreateDBOnSpecificShard);
MONGO_FAIL_POINT_DEFINE(hangDollarOutAfterInsert);

REGISTER_AGG_STAGE_MAPPING(outStage, DocumentSourceOut::id, documentSourceOutToStageFn)

void OutStage::doDispose() {
    if (_tmpCleanUpState == OutCleanUpProgress::kComplete) {
        return;
    }

    // Make sure we drop the temp collection(s) if anything goes wrong.
    // Errors are ignored here because nothing can be done about them. Additionally, if
    // this fails and the collection is left behind, it will be cleaned up next time the
    // server is started.
    auto cleanupClient =
        pExpCtx->getOperationContext()->getService()->makeClient("$out_replace_coll_cleanup");
    AlternativeClientRegion acr(cleanupClient);

    // Create a new operation context so that any interrupts on the current operation
    // will not affect the dropCollection operation below.
    auto cleanupOpCtx = cc().makeOperationContext();
    DocumentSourceWriteBlock writeBlock(cleanupOpCtx.get());
    auto dropCollectionCmd = [&](NamespaceString dropNs) {
        try {
            pExpCtx->getMongoProcessInterface()->dropTempCollection(cleanupOpCtx.get(), dropNs);
        } catch (const DBException& e) {
            LOGV2_WARNING(7466203,
                          "Unexpected error dropping temporary $out collection; drop will complete "
                          "on next server restart",
                          "error"_attr = redact(e.toString()),
                          "coll"_attr = dropNs);
        };
    };

    switch (_tmpCleanUpState) {
        case OutCleanUpProgress::kTmpCollExists:
            dropCollectionCmd(_tempNs);
            // SERVER-112874: For viewful timeseries, if the primary stepped down and the
            // temp buckets collection was dropped, inserts may have implicitly created a
            // non-timeseries collection at the view namespace. Drop it to avoid leaving
            // temp collections behind.
            // TODO SERVER-111600: Remove this once 9.0 is LTS and all timeseries are viewless.
            if (_tempNs.isTimeseriesBucketsCollection()) {
                dropCollectionCmd(_tempNs.getTimeseriesViewNamespace());
            }
            break;
        case OutCleanUpProgress::kRenameComplete:
            // For legacy time-series collections, since we haven't created a view in this state, we
            // must drop the buckets collection.
            // TODO SERVER-92272 Update this to only drop the collection iff a time-series view
            // doesn't exist.
            if (_outIsLegacyTimeseries) {
                auto collType = pExpCtx->getMongoProcessInterface()->getCollectionType(
                    cleanupOpCtx.get(), _outputNs);
                if (collType != query_shape::CollectionType::kTimeseries) {
                    dropCollectionCmd(_outputNs.makeTimeseriesBucketsNamespace());
                }
            }
            [[fallthrough]];
        case OutCleanUpProgress::kViewCreatedIfNeeded:
            // This state indicates that the rename succeeded, but 'dropTempCollection' hasn't
            // finished. For sharding we must also explicitly call 'dropTempCollection' on the
            // temporary namespace to remove the namespace from the list of in-use temporary
            // collections.
            dropCollectionCmd(_tempNs);
            break;
        default:
            MONGO_UNREACHABLE;
            break;
    }
}

void OutStage::retrieveOriginalOutCollInfo() {
    try {
        // Save the original collection options and index specs so we can check they didn't change
        // during computation.
        auto originalOptions = pExpCtx->getMongoProcessInterface()->getCollectionOptions(
            pExpCtx->getOperationContext(), _outputNs);
        auto isLegacyTimeseries = false;

        // TODO SERVER-111600: remove this translation logic once 9.0 is last LTS and all timeseries
        // are viewless
        //
        // A legacy timeseries view has "timeseries" but no "uuid" in its options. We need to
        // resolve it to the underlying system.buckets collection. Retries handle the race where
        // the buckets collection is dropped/recreated between lookups.
        auto isLegacyTimeseriesView = [](const BSONObj& opts) {
            return !opts.isEmpty() && opts.hasField("timeseries") && !opts.hasField("uuid");
        };

        for (int remainingAttempts = kMaxCatalogRetryAttempts;
             isLegacyTimeseriesView(originalOptions);) {
            uassert(11281630,
                    fmt::format("Exhausted all attempts to resolve legacy timeseries view to "
                                "underlying system buckets collection for namespace '{}'",
                                _outputNs.toStringForErrorMsg()),
                    remainingAttempts-- > 0);

            const auto bucketsNss = _outputNs.makeTimeseriesBucketsNamespace();
            auto bucketsOptions = pExpCtx->getMongoProcessInterface()->getCollectionOptions(
                pExpCtx->getOperationContext(), bucketsNss);
            if (bucketsOptions.isEmpty()) {
                // The system.buckets collection disappeared — re-read the view.
                originalOptions = pExpCtx->getMongoProcessInterface()->getCollectionOptions(
                    pExpCtx->getOperationContext(), _outputNs);
            } else {
                originalOptions = std::move(bucketsOptions);
                isLegacyTimeseries = true;
            }
        }

        if (!originalOptions.isEmpty()) {
            // When rawData operations are not supported (FCV < 8.2), getIndexSpecs cannot
            // resolve timeseries indexes through the user-facing namespace, so we must
            // explicitly target the underlying system.buckets collection.
            auto outputNsForFetchingIndexes = originalOptions.hasField("timeseries") &&
                    !gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
                        VersionContext::getDecoration(pExpCtx->getOperationContext()),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())
                ? _outputNs.makeTimeseriesBucketsNamespace()
                : _outputNs;
            auto originalIndexes =
                pExpCtx->getMongoProcessInterface()->getIndexSpecs(pExpCtx->getOperationContext(),
                                                                   outputNsForFetchingIndexes,
                                                                   false /* includeBuildUUIDs */);

            // The uuid field is considered an option, but cannot be passed to createCollection.
            originalOptions = originalOptions.removeField("uuid");

            // Use the original options to correctly determine if we are writing to a time-series
            // collection. We must set _originalOutCollInfo before calling validateTimeseries(),
            // since it reads _originalOutCollInfo->options.
            _originalOutCollInfo.emplace(OriginalCollectionInfo{
                std::move(originalOptions), std::move(originalIndexes), isLegacyTimeseries});
        }
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        LOGV2_DEBUG(7585601,
                    5,
                    "Database for $out target collection doesn't exist. Assuming default indexes "
                    "and options");
    }
}

void OutStage::createTemporaryCollection() {
    auto createCommandOptions = [&] {
        BSONObjBuilder builder;
        if (_timeseries) {
            // Append the original collection options without the 'validator' and 'clusteredIndex'
            // fields since these fields are invalid with the 'timeseries' field and will be
            // recreated when the timeseries collection is created.
            !_originalOutCollInfo
                ? builder << DocumentSourceOutSpec::kTimeseriesFieldName << _timeseries->toBSON()
                : builder.appendElementsUnique(_originalOutCollInfo->options.removeFields(
                      StringDataSet{"clusteredIndex", "validator"}));
        } else if (_originalOutCollInfo) {
            builder.appendElementsUnique(_originalOutCollInfo->options);
        }
        return builder.obj();
    }();

    auto targetShard = [&]() -> boost::optional<ShardId> {
        if (auto fpTarget = outImplictlyCreateDBOnSpecificShard.scoped();
            // Used for consistently picking a shard in testing.
            MONGO_unlikely(fpTarget.isActive())) {
            return ShardId(fpTarget.getData()["shardId"].String());
        } else {
            // If the output collection exists, we should create the temp collection on the shard
            // that owns the output collection.
            return _mergeShardId;
        }
    }();

    _outIsLegacyTimeseries = [&] {
        if (!_timeseries) {
            return false;
        }
        // If the original out collection exists, the temporary collection should be of the same
        // type.
        if (_originalOutCollInfo) {
            return _originalOutCollInfo->isLegacyTimeseries;
        }
        // If the original out collection does not exist, we create the temporary either as viewful
        // or viewless according to the feature flag.
        return !gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
            VersionContext(serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    }();

    //  Note that this temporary collection name is used by MongoMirror and thus should not be
    //  changed without consultation.
    _tempNs = makeBucketNsIfLegacyTimeseries(NamespaceStringUtil::deserialize(
        _outputNs.dbName(),
        str::stream() << NamespaceString::kOutTmpCollectionPrefix << UUID::gen()));

    // Set the enum state to 'kTmpCollExists' first, because 'createTempCollection' can throw
    // after constructing the collection.
    _tmpCleanUpState = OutCleanUpProgress::kTmpCollExists;

    for (int remainingAttempts = kMaxCatalogRetryAttempts; remainingAttempts--;) {
        try {
            LOGV2_DEBUG(11281650,
                        3,
                        "Creating $out temp collection",
                        "isTimeseriesOp"_attr = !!_timeseries,
                        "originalOutExists"_attr = !!_originalOutCollInfo,
                        "originalOutIsLegacyTimeseries"_attr =
                            _originalOutCollInfo && _originalOutCollInfo->isLegacyTimeseries,
                        "tempNss"_attr = _tempNs,
                        "outIsLegacyTimeseries"_attr = _outIsLegacyTimeseries);

            pExpCtx->getMongoProcessInterface()->createTempCollection(
                pExpCtx->getOperationContext(), _tempNs, createCommandOptions, targetShard);
            break;
        } catch (DBException& ex) {
            if (ex.code() != timeseries::kLegacyTimeseriesTempCollectionCreationError) {
                ex.addContext("Creation of temporary $out collection failed");
                throw;
            }
            if (remainingAttempts <= 0) {
                ex.addContext("Exhausted retry attempts while creating $out temporary collection");
                throw;
            }
            // The output collection's timeseries type may have changed during an FCV
            // transition. Toggle and retry with the translated namespace.
            _outIsLegacyTimeseries = !_outIsLegacyTimeseries;
            _tempNs = _tempNs.isTimeseriesBucketsCollection()
                ? _tempNs.getTimeseriesViewNamespace()
                : _tempNs.makeTimeseriesBucketsNamespace();
        }
    }

    // If we started with an existing out collection that was either viewful or viewless timeseries
    // and then we discover that the temporary was created as a different timeseries type, it means
    // an FCV upgrade/downgrade happened in the middle. Raise a retriable error.
    uassert(ErrorCodes::InterruptedDueToTimeseriesUpgradeDowngrade,
            fmt::format("The metadata of timeseries collection '{}' changed due to an "
                        "FCV transition during the execution of $out operation",
                        _outputNs.toStringForErrorMsg()),
            !_originalOutCollInfo ||
                _originalOutCollInfo->isLegacyTimeseries == _outIsLegacyTimeseries);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitAfterTempCollectionCreation,
        pExpCtx->getOperationContext(),
        "outWaitAfterTempCollectionCreation",
        []() {
            LOGV2(20901,
                  "Hanging aggregation due to 'outWaitAfterTempCollectionCreation' failpoint");
        });

    // The create command does not return the UUID of the newly created collection thus we need to
    // explicitly fetch it.
    retrieveTemporaryCollectionUUID();

    LOGV2_DEBUG(11281651,
                3,
                "Created $out temp collection",
                "isTimeseriesOp"_attr = !!_timeseries,
                "originalOutExists"_attr = !!_originalOutCollInfo,
                "originalOutIsLegacyTimeseries"_attr =
                    _originalOutCollInfo && _originalOutCollInfo->isLegacyTimeseries,
                "tempNss"_attr = _tempNs,
                "tempUUID"_attr = *_tempNsUUID,
                "outIsLegacyTimeseries"_attr = _outIsLegacyTimeseries);

    if (!_originalOutCollInfo || _originalOutCollInfo->indexes.empty()) {
        return;
    }

    // Copy the indexes of the output collection to the temp collection.
    try {
        auto targetNsForCreateIndex = _tempNs;
        if (gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
                VersionContext::getDecoration(pExpCtx->getOperationContext()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            // Since the indexes we get from the target collection are already translated to be on
            // the fields of the bucket documents, we need to perform the recreation of those
            // indexes on the temp collection as a rawData operation to avoid erroneously
            // translating the indexes a second time.
            isRawDataOperation(pExpCtx->getOperationContext()) = true;

            if (_outIsLegacyTimeseries) {
                // If we can use rawData then we always use the main timeseries namespace, never the
                // internal system.buckets.
                targetNsForCreateIndex = _tempNs.getTimeseriesViewNamespace();
            }
        }

        pExpCtx->getMongoProcessInterface()->createIndexesOnEmptyCollection(
            pExpCtx->getOperationContext(), targetNsForCreateIndex, _originalOutCollInfo->indexes);

        // Reset rawData to false for the rest of the operation
        isRawDataOperation(pExpCtx->getOperationContext()) = false;
    } catch (DBException& ex) {
        ex.addContext("Copying indexes for $out failed");
        throw;
    }
}

void OutStage::retrieveTemporaryCollectionUUID() {
    auto targetTempNs = _tempNs;
    for (int remainingAttempts = kMaxCatalogRetryAttempts; remainingAttempts--;) {
        boost::optional<ListCollectionsReplyItem> optCollInfo;
        try {
            optCollInfo = pExpCtx->getMongoProcessInterface()->getCollectionInfoFromPrimary(
                pExpCtx->getOperationContext(), targetTempNs);
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // optCollInfo remains boost::none
        } catch (DBException& ex) {
            ex.addContext("Error while fetching $out temporary collection UUID");
            throw;
        }

        if (optCollInfo && optCollInfo->getInfo() && optCollInfo->getInfo()->getUuid()) {
            _tempNsUUID = optCollInfo->getInfo()->getUuid();
            uassert(ErrorCodes::InterruptedDueToTimeseriesUpgradeDowngrade,
                    fmt::format(
                        "The metadata of temporary timeseries collection '{}' changed due to an "
                        "FCV transition during the execution of $out operation",
                        _tempNs.toStringForErrorMsg()),
                    targetTempNs == _tempNs);
            return;
        }

        LOGV2_DEBUG(11281692,
                    2,
                    "$out temp collection info retrieval attempt",
                    "remainingAttempts"_attr = remainingAttempts,
                    "targetTempNs"_attr = targetTempNs.toStringForErrorMsg(),
                    "listCollectionReplyItem"_attr = optCollInfo);

        if (_timeseries && remainingAttempts > 0) {
            // If this is a timeseries $out, the temp collection may have been converted from
            // viewful to viewless (or vice-versa) during an FCV transition. Check if the
            // translated namespace holds the temp collection.
            targetTempNs = targetTempNs.isTimeseriesBucketsCollection()
                ? targetTempNs.getTimeseriesViewNamespace()
                : targetTempNs.makeTimeseriesBucketsNamespace();
            continue;
        }

        if (!optCollInfo) {
            uasserted(
                ErrorCodes::NamespaceNotFound,
                fmt::format(
                    "Temporary collection '{}' disappeared during execution of $out collection",
                    _tempNs.toStringForErrorMsg()));
        } else {
            uasserted(11281693,
                      fmt::format("Found unexpected metadata for $out temporary collection. "
                                  "Collection Namespace: '{}', Collection Info: {}",
                                  targetTempNs.toStringForErrorMsg(),
                                  optCollInfo->toBSON().toString()));
        }
    }
}

void OutStage::initialize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
    // We will create a temporary collection with the same indexes and collection options as the
    // target collection if it exists. We will write all results into a temporary collection, then
    // rename the temporary collection to be the target collection once we are done.

    retrieveOriginalOutCollInfo();

    _timeseries = validateTimeseries();

    // Check if it's capped to make sure we have a chance of succeeding before we do all the
    // work. If the collection becomes capped during processing, the collection options will
    // have changed, and the $out will fail.
    uassert(17152,
            fmt::format("namespace '{}' is capped so it can't be used for {}",
                        _outputNs.toStringForErrorMsg(),
                        _commonStats.stageTypeStr),
            !_originalOutCollInfo || _originalOutCollInfo->options["capped"].eoo());

    uassert(7406100,
            "$out to time-series collections is only supported on FCV greater than or equal to 7.1",
            feature_flags::gFeatureFlagAggOutTimeseries.isEnabled() || !_timeseries);

    createTemporaryCollection();
}

void OutStage::finalize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
    uassert(7406101,
            "$out to time-series collections is only supported on FCV greater than or equal to 7.1",
            feature_flags::gFeatureFlagAggOutTimeseries.isEnabled() || !_timeseries);

    // Rename the temporary collection to the namespace the user requested, and drop the target
    // collection if $out is writing to a collection that exists.
    renameTemporaryCollection();

    // The rename has succeeded, if the collection is legacy time-series, try to create the view.
    // Creating the view must happen immediately after the rename. We cannot guarantee that both the
    // rename and view creation for legacy time-series will succeed if there is an unclean shutdown.
    // This could lead us to an unsupported state (a buckets collection with no view). To minimize
    // the chance this happens, we should ensure that view creation is tried immediately after the
    // rename succeeds.
    if (_outIsLegacyTimeseries) {
        createTimeseriesView();
    }

    // The rename succeeded, so the temp collection no longer exists. Call 'dropTempCollection'
    // anyway to ensure that we remove it from the list of in-use temporary collections that will be
    // dropped on stepup (relevant on sharded clusters).
    _tmpCleanUpState = OutCleanUpProgress::kViewCreatedIfNeeded;
    pExpCtx->getMongoProcessInterface()->dropTempCollection(pExpCtx->getOperationContext(),
                                                            _tempNs);

    _tmpCleanUpState = OutCleanUpProgress::kComplete;
}

void OutStage::flush(BatchedCommandRequest bcr, BatchedObjects batch) {
    DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());

    auto insertCommand = bcr.extractInsertRequest();
    insertCommand->setDocuments(std::move(batch));
    auto targetEpoch = boost::none;

    if (_timeseries) {
        for (const auto& writeError : pExpCtx->getMongoProcessInterface()->insertTimeseries(
                 pExpCtx,
                 _tempNs.isTimeseriesBucketsCollection() ? _tempNs.getTimeseriesViewNamespace()
                                                         : _tempNs,
                 std::move(insertCommand),
                 _writeConcern,
                 targetEpoch)) {
            uassertStatusOK(writeError.getStatus());
        }
    } else {
        // Use the UUID to catch a mismatch if the temp collection was dropped and recreated.
        // Timeseries will detect this as inserts don't implicitly
        // create the collection. Inserts with uuid are not supported with apiStrict, so there is a
        // secondary check at the rename when apiStrict is true.
        if (!APIParameters::get(pExpCtx->getOperationContext()).getAPIStrict().value_or(false)) {
            tassert(8085300, "No uuid found for $out temporary namespace", _tempNsUUID);
            insertCommand->getWriteCommandRequestBase().setCollectionUUID(_tempNsUUID);
        }
        try {
            for (const auto& writeError : pExpCtx->getMongoProcessInterface()->insert(
                     pExpCtx, _tempNs, std::move(insertCommand), _writeConcern, targetEpoch)) {
                uassertStatusOK(writeError.getStatus());
            }

        } catch (ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& ex) {
            ex.addContext(
                str::stream()
                << "$out cannot complete as the temp collection was dropped while executing");
            throw;
        }
    }

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDollarOutAfterInsert,
        pExpCtx->getOperationContext(),
        "hangDollarOutAfterInsert",
        []() {
            LOGV2(8085302, "Hanging aggregation due to 'hangDollarOutAfterInsert' failpoint");
        });
}

BatchedCommandRequest OutStage::makeBatchedWriteRequest() const {
    const auto& nss =
        _tempNs.isTimeseriesBucketsCollection() ? _tempNs.getTimeseriesViewNamespace() : _tempNs;
    return makeInsertCommand(nss, pExpCtx->getBypassDocumentValidation());
}

void OutStage::waitWhileFailPointEnabled() {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWhileBuildingDocumentSourceOutBatch,
        pExpCtx->getOperationContext(),
        "hangWhileBuildingDocumentSourceOutBatch",
        []() {
            LOGV2(20902,
                  "Hanging aggregation due to 'hangWhileBuildingDocumentSourceOutBatch' failpoint");
        });
}

void OutStage::checkTemporaryCollectionUUIDNotChanged() {
    tassert(8085303, "No uuid found for $out temporary namespace", _tempNsUUID);
    const auto& tempCollEntry = [&] {
        try {
            return pExpCtx->getMongoProcessInterface()->getCollectionInfoFromPrimary(
                pExpCtx->getOperationContext(),
                NamespaceStringOrUUID(_tempNs.dbName(), *_tempNsUUID));
        } catch (DBException& ex) {
            if (ex.code() == ErrorCodes::NamespaceNotFound) {
                ex.addContext("Temporary collection has been dropped during execution of $out");
            } else {
                ex.addContext("Error fetching temporary $out collection information");
            }
            throw;
        }
    }();

    const auto& resolvedNs =
        NamespaceStringUtil::deserialize(_tempNs.dbName(), tempCollEntry.getName());

    if (resolvedNs != _tempNs) {

        // TODO SERVER-111600: Remove this function once 9.0 is last LTS and all timeseries are
        // viewless
        if (resolvedNs.isTimeseriesBucketsCollection() || _tempNs.isTimeseriesBucketsCollection()) {
            uasserted(
                ErrorCodes::InterruptedDueToTimeseriesUpgradeDowngrade,
                fmt::format("Operation on collection '{}' was interrupted due to a time-series "
                            "metadata change during FCV transition. Retry the operation.",
                            _tempNs.toStringForErrorMsg()));
        }
        uasserted(11281620,
                  fmt::format("Temporary collection has been renamed during execution of "
                              "$out. From '{}' to '{}'",
                              _tempNs.toStringForErrorMsg(),
                              resolvedNs.toStringForErrorMsg()));
    }
}

void OutStage::renameTemporaryCollection() {
    // If the collection is legacy time-series, we must rename to the "real" buckets collection.
    const NamespaceString& outputNs = makeBucketNsIfLegacyTimeseries(_outputNs);

    checkTemporaryCollectionUUIDNotChanged();

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitBeforeTempCollectionRename,
        pExpCtx->getOperationContext(),
        "outWaitBeforeTempCollectionRename",
        []() {
            LOGV2(7585602,
                  "Hanging aggregation due to 'outWaitBeforeTempCollectionRename' failpoint");
        });

    LOGV2_DEBUG(11281653,
                3,
                "Renaming temporary $out collection",
                "isTimeseriesOp"_attr = !!_timeseries,
                "originalOutExists"_attr = !!_originalOutCollInfo,
                "originalOutIsLegacyTimeseries"_attr =
                    _originalOutCollInfo && _originalOutCollInfo->isLegacyTimeseries,
                "tempNss"_attr = _tempNs,
                "tempUUID"_attr = *_tempNsUUID,
                "outNss"_attr = outputNs,
                "outIsLegacyTimeseries"_attr = _outIsLegacyTimeseries);

    try {
        pExpCtx->getMongoProcessInterface()->renameIfOptionsAndIndexesHaveNotChanged(
            pExpCtx->getOperationContext(),
            _tempNs,
            outputNs,
            true /* dropTarget */,
            false /* stayTemp */,
            _originalOutCollInfo ? _originalOutCollInfo->options : BSONObj{},
            _originalOutCollInfo ? _originalOutCollInfo->indexes : std::vector<BSONObj>{});
    } catch (DBException& ex) {
        ex.addContext("Failed to rename temporary $out collection");
        throw;
    }
}

// TODO SERVER-111600: Remove this function once 9.0 is last LTS and all timeseries are viewless
void OutStage::createTimeseriesView() {
    _tmpCleanUpState = OutCleanUpProgress::kRenameComplete;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitAfterTempCollectionRenameBeforeView,
        pExpCtx->getOperationContext(),
        "outWaitAfterTempCollectionRenameBeforeView",
        []() {
            LOGV2(8961400,
                  "Hanging aggregation due to 'outWaitAfterTempCollectionRenameBeforeView' "
                  "failpoint");
        });

    BSONObjBuilder cmd;
    cmd << "create" << _outputNs.coll();
    cmd << DocumentSourceOutSpec::kTimeseriesFieldName << _timeseries->toBSON();
    pExpCtx->getMongoProcessInterface()->createTimeseriesView(
        pExpCtx->getOperationContext(), _outputNs, cmd.done(), *_timeseries);
}

// TODO SERVER-111600: Remove this function once 9.0 is last LTS and all timeseries are viewless
NamespaceString OutStage::makeBucketNsIfLegacyTimeseries(const NamespaceString& ns) {
    return _outIsLegacyTimeseries ? ns.makeTimeseriesBucketsNamespace() : ns;
}

std::shared_ptr<TimeseriesOptions> OutStage::validateTimeseries() {
    const BSONElement targetTimeseriesElement =
        _originalOutCollInfo ? _originalOutCollInfo->options["timeseries"] : BSONElement{};
    std::shared_ptr<TimeseriesOptions> targetTSOpts;
    if (targetTimeseriesElement) {
        tassert(
            9072001, "Invalid time-series options received", targetTimeseriesElement.isABSONObj());
        targetTSOpts = std::make_shared<TimeseriesOptions>(TimeseriesOptions::parseOwned(
            targetTimeseriesElement.Obj(), IDLParserContext("TimeseriesOptions")));
    }

    // If the user did not specify the 'timeseries' option in the input, but the target
    // namespace is a time-series collection, then we will use the target collection time-series
    // options, and treat this operation as a write to time-series collection.
    if (!_timeseries) {
        return targetTSOpts;
    }

    // If the user specified 'timeseries' options, the target namespace must be a time-series
    // collection. Note that the result of 'getCollectionType' can become stale at
    // anytime and shouldn't be referenced at any other point. $out should account for
    // concurrent view or collection creation during each step of its execution.
    uassert(7268700,
            "Cannot create a time-series collection from a non time-series collection or view.",
            targetTSOpts || !_originalOutCollInfo);

    // If the user did specify 'timeseries' options and the target namespace is a time-series
    // collection, then the time-series options should match.
    uassert(7406103,
            str::stream() << "Time-series options inputted must match the existing time-series "
                             "collection. Received: "
                          << _timeseries->toBSON().toString()
                          << "Found: " << targetTimeseriesElement.toString(),
            !targetTSOpts || timeseries::optionsAreEqual(*_timeseries, *targetTSOpts));

    return _timeseries;
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
