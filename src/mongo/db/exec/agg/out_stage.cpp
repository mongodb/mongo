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
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/writer_util.h"
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
                          "Unexpected error dropping temporary collection; drop will complete "
                          "on next server restart",
                          "error"_attr = redact(e.toString()),
                          "coll"_attr = dropNs);
        };
    };

    switch (_tmpCleanUpState) {
        case OutCleanUpProgress::kTmpCollExists:
            dropCollectionCmd(_tempNs);
            break;
        case OutCleanUpProgress::kRenameComplete:
            // For time-series collections, since we haven't created a view in this state, we
            // must drop the buckets collection.
            // TODO SERVER-92272 Update this to only drop the collection iff a time-series view
            // doesn't exist.
            if (_timeseries) {
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

void OutStage::createTemporaryCollection() {
    BSONObjBuilder createCommandOptions;
    if (_timeseries) {
        // Append the original collection options without the 'validator' and 'clusteredIndex'
        // fields since these fields are invalid with the 'timeseries' field and will be
        // recreated when the buckets collection is created.
        _originalOutOptions.isEmpty()
            ? createCommandOptions << DocumentSourceOutSpec::kTimeseriesFieldName
                                   << _timeseries->toBSON()
            : createCommandOptions.appendElementsUnique(
                  _originalOutOptions.removeFields(StringDataSet{"clusteredIndex", "validator"}));
    } else {
        createCommandOptions.appendElementsUnique(_originalOutOptions);
    }

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

    // Set the enum state to 'kTmpCollExists' first, because 'createTempCollection' can throw
    // after constructing the collection.
    _tmpCleanUpState = OutCleanUpProgress::kTmpCollExists;
    pExpCtx->getMongoProcessInterface()->createTempCollection(
        pExpCtx->getOperationContext(), _tempNs, createCommandOptions.done(), targetShard);
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitAfterTempCollectionCreation,
        pExpCtx->getOperationContext(),
        "outWaitAfterTempCollectionCreation",
        []() {
            LOGV2(20901,
                  "Hanging aggregation due to 'outWaitAfterTempCollectionCreation' failpoint");
        });
}

void OutStage::initialize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
    // We will create a temporary collection with the same indexes and collection options as the
    // target collection if it exists. We will write all results into a temporary collection, then
    // rename the temporary collection to be the target collection once we are done.

    try {
        // Save the original collection options and index specs so we can check they didn't change
        // during computation. For time-series collections, these should be run on the buckets
        // namespace.
        _originalOutOptions =
            // The uuid field is considered an option, but cannot be passed to createCollection.
            pExpCtx->getMongoProcessInterface()
                ->getCollectionOptions(pExpCtx->getOperationContext(),
                                       makeBucketNsIfTimeseries(_outputNs))
                .removeField("uuid");

        // Use '_originalOutOptions' to correctly determine if we are writing to a time-series
        // collection.
        _timeseries = validateTimeseries();
        _originalIndexes =
            pExpCtx->getMongoProcessInterface()->getIndexSpecs(pExpCtx->getOperationContext(),
                                                               makeBucketNsIfTimeseries(_outputNs),
                                                               false /* includeBuildUUIDs */);

        // Check if it's capped to make sure we have a chance of succeeding before we do all the
        // work. If the collection becomes capped during processing, the collection options will
        // have changed, and the $out will fail.
        uassert(17152,
                fmt::format("namespace '{}' is capped so it can't be used for {}",
                            _outputNs.toStringForErrorMsg(),
                            _commonStats.stageTypeStr),
                _originalOutOptions["capped"].eoo());
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        LOGV2_DEBUG(7585601,
                    5,
                    "Database for $out target collection doesn't exist. Assuming default indexes "
                    "and options");
    }

    //  Note that this temporary collection name is used by MongoMirror and thus should not be
    //  changed without consultation.
    _tempNs = makeBucketNsIfTimeseries(NamespaceStringUtil::deserialize(
        _outputNs.dbName(),
        str::stream() << NamespaceString::kOutTmpCollectionPrefix << UUID::gen()));

    uassert(7406100,
            "$out to time-series collections is only supported on FCV greater than or equal to 7.1",
            feature_flags::gFeatureFlagAggOutTimeseries.isEnabled() || !_timeseries);

    createTemporaryCollection();

    // Save the collection UUID to detect if it was dropped during execution. Timeseries will detect
    // this when inserting as it doesn't implicity create collections on insert.
    if (!_timeseries) {
        _tempNsUUID = pExpCtx->getMongoProcessInterface()->fetchCollectionUUIDFromPrimary(
            pExpCtx->getOperationContext(), _tempNs);
    }

    if (_originalIndexes.empty()) {
        return;
    }

    // Copy the indexes of the output collection to the temp collection.
    // Note that on timeseries collections, indexes are to be created on the buckets collection.
    try {
        std::vector<BSONObj> tempNsIndexes = {std::begin(_originalIndexes),
                                              std::end(_originalIndexes)};
        pExpCtx->getMongoProcessInterface()->createIndexesOnEmptyCollection(
            pExpCtx->getOperationContext(), _tempNs, tempNsIndexes);
    } catch (DBException& ex) {
        ex.addContext("Copying indexes for $out failed");
        throw;
    }
}

void OutStage::finalize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
    uassert(7406101,
            "$out to time-series collections is only supported on FCV greater than or equal to 7.1",
            feature_flags::gFeatureFlagAggOutTimeseries.isEnabled() || !_timeseries);

    // Rename the temporary collection to the namespace the user requested, and drop the target
    // collection if $out is writing to a collection that exists.
    renameTemporaryCollection();

    // The rename has succeeded, if the collection is time-series, try to create the view. Creating
    // the view must happen immediately after the rename. We cannot guarantee that both the rename
    // and view creation for time-series will succeed if there is an unclean shutdown. This could
    // lead us to an unsupported state (a buckets collection with no view). To minimize the chance
    // this happens, we should ensure that view creation is tried immediately after the rename
    // succeeds.
    if (_timeseries) {
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
        uassertStatusOK(pExpCtx->getMongoProcessInterface()->insertTimeseries(
            pExpCtx, _tempNs, std::move(insertCommand), _writeConcern, targetEpoch));
    } else {
        // Use the UUID to catch a mismatch if the temp collection was dropped and recreated.
        // Timeseries will detect this as inserts into the buckets collection don't implicitly
        // create the collection. Inserts with uuid are not supported with apiStrict, so there is a
        // secondary check at the rename when apiStrict is true.
        if (!APIParameters::get(pExpCtx->getOperationContext()).getAPIStrict().value_or(false)) {
            tassert(8085300, "No uuid found for $out temporary namespace", _tempNsUUID);
            insertCommand->getWriteCommandRequestBase().setCollectionUUID(_tempNsUUID);
        }
        try {
            uassertStatusOK(pExpCtx->getMongoProcessInterface()->insert(
                pExpCtx, _tempNs, std::move(insertCommand), _writeConcern, targetEpoch));

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

void OutStage::renameTemporaryCollection() {
    // If the collection is time-series, we must rename to the "real" buckets collection.
    const NamespaceString& outputNs = makeBucketNsIfTimeseries(_outputNs);

    // Use the UUID to catch a mismatch if the temp collection was dropped and recreated in case of
    // stepdown. Timeseries has it's own handling for this case as the dropped temp collection isn't
    // implicitly recreated.
    if (!_timeseries) {
        tassert(8085301, "No uuid found for $out temporary namespace", _tempNsUUID);
        const UUID currentTempNsUUID =
            pExpCtx->getMongoProcessInterface()->fetchCollectionUUIDFromPrimary(
                pExpCtx->getOperationContext(), _tempNs);
        uassert((CollectionUUIDMismatchInfo{
                    _tempNs.dbName(), currentTempNsUUID, std::string{_tempNs.coll()}, boost::none}),
                "$out cannot complete as the temp collection was dropped while executing",
                currentTempNsUUID == _tempNsUUID);
    }

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitBeforeTempCollectionRename,
        pExpCtx->getOperationContext(),
        "outWaitBeforeTempCollectionRename",
        []() {
            LOGV2(7585602,
                  "Hanging aggregation due to 'outWaitBeforeTempCollectionRename' failpoint");
        });
    pExpCtx->getMongoProcessInterface()->renameIfOptionsAndIndexesHaveNotChanged(
        pExpCtx->getOperationContext(),
        _tempNs,
        outputNs,
        true /* dropTarget */,
        false /* stayTemp */,
        _originalOutOptions,
        _originalIndexes);
}

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

NamespaceString OutStage::makeBucketNsIfTimeseries(const NamespaceString& ns) {
    return _timeseries ? ns.makeTimeseriesBucketsNamespace() : ns;
}

std::shared_ptr<TimeseriesOptions> OutStage::validateTimeseries() {
    const BSONElement targetTimeseriesElement = _originalOutOptions["timeseries"];
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
        // Must update '_originalOutOptions' to be on the buckets namespace, since previously we
        // didn't know we will be writing a time-series collection.
        if (targetTSOpts) {
            _originalOutOptions =
                pExpCtx->getMongoProcessInterface()
                    ->getCollectionOptions(pExpCtx->getOperationContext(),
                                           _outputNs.makeTimeseriesBucketsNamespace())
                    .removeField("uuid");
        }
        return targetTSOpts;
    }

    // If the user specified 'timeseries' options, the target namespace must be a time-series
    // collection. Note that the result of 'getCollectionType' can become stale at
    // anytime and shouldn't be referenced at any other point. $out should account for
    // concurrent view or collection creation during each step of its execution.
    uassert(7268700,
            "Cannot create a time-series collection from a non time-series collection or view.",
            targetTSOpts ||
                pExpCtx->getMongoProcessInterface()->getCollectionType(
                    pExpCtx->getOperationContext(), _outputNs) ==
                    query_shape::CollectionType::kNonExistent);

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
