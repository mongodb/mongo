// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/timeseries/timeseries_collmod.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/shard_role/shard_catalog/coll_mod.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace timeseries {

std::unique_ptr<CollMod> makeTimeseriesBucketsCollModCommand(TimeseriesOptions& timeseriesOptions,
                                                             const CollMod& origCmd,
                                                             bool isLegacyTimeseries) {
    auto index = origCmd.getIndex();
    if (index && index->getKeyPattern()) {
        auto bucketsIndexSpecWithStatus = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
            timeseriesOptions, *index->getKeyPattern());

        uassert(ErrorCodes::IndexNotFound,
                str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                              << " Command request: " << redact(origCmd.toBSON()),
                bucketsIndexSpecWithStatus.isOK());

        index->setKeyPattern(std::move(bucketsIndexSpecWithStatus.getValue()));
    }

    const auto& origNs = origCmd.getNamespace();
    auto ns = isLegacyTimeseries ? origNs.makeTimeseriesBucketsNamespace() : origNs;
    CollModRequest request;
    request.setIndex(index);
    request.setValidator(origCmd.getValidator());
    request.setValidationLevel(origCmd.getValidationLevel());
    request.setValidationAction(origCmd.getValidationAction());
    request.setViewOn(origCmd.getViewOn());
    request.setPipeline(origCmd.getPipeline());
    request.setChangeStreamPreAndPostImages(origCmd.getChangeStreamPreAndPostImages());
    request.setExpireAfterSeconds(origCmd.getExpireAfterSeconds());
    request.setTimeseries(origCmd.getTimeseries());
    request.setTimeseriesBucketsMayHaveMixedSchemaData(
        origCmd.getTimeseriesBucketsMayHaveMixedSchemaData());
    request.setDryRun(origCmd.getDryRun());
    auto cmd = std::make_unique<CollMod>(ns);
    cmd->setCollModRequest(request);
    return cmd;
}

std::unique_ptr<CollMod> makeTimeseriesViewCollModCommand(TimeseriesOptions& timeseriesOptions,
                                                          const CollMod& origCmd) {
    const auto& ns = origCmd.getNamespace();

    auto& tsMod = origCmd.getTimeseries();
    if (tsMod) {
        auto res = timeseries::applyTimeseriesOptionsModifications(timeseriesOptions, *tsMod);
        if (res.isOK()) {
            auto& [newOptions, changed] = res.getValue();
            if (changed) {
                auto cmd = std::make_unique<CollMod>(ns);
                constexpr bool asArray = false;
                std::vector<BSONObj> pipeline = {
                    timeseries::generateViewPipeline(newOptions, asArray)};
                CollModRequest viewRequest;
                viewRequest.setPipeline(std::move(pipeline));
                cmd->setCollModRequest(viewRequest);
                return cmd;
            }
        }
    }

    return {};
}

Status processCollModCommandWithTimeSeriesTranslation(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const CollMod& cmd,
                                                      bool performViewChange,
                                                      BSONObjBuilder* result) {
    auto [namespaceExists, timeseriesOptions, isLegacyTimeseries] = [&]() -> auto {
        // TODO SERVER-105548 switch back to acquireCollection once 9.0 becomes last LTS
        auto [collAcq, wasNssTranslatedToBucket] =
            timeseries::acquireCollectionOrViewWithBucketsLookup(
                opCtx,
                CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx,
                    cmd.getNamespace(),
                    cmd.getCollectionUUID(),
                    AcquisitionPrerequisites::OperationType::kRead),
                LockMode::MODE_IS);

        auto namespaceExists = collAcq.collectionExists() || collAcq.isView();
        auto tsOptions = collAcq.collectionExists()
            ? collAcq.getCollectionPtr()->getTimeseriesOptions()
            : boost::none;
        return std::make_tuple(namespaceExists, tsOptions, wasNssTranslatedToBucket);
    }();

    // Fail early if the collection does not exist: We do not take the DDL lock on createCollection,
    // so the collection may get later created either as a regular collection or timeseries.
    // Therefore, we can not decide whether to apply timeseries translation or not.
    if (!namespaceExists) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "namespace does not exist: " << nss.toStringForErrorMsg());
    }

    if (!timeseriesOptions) {
        return processCollModCommand(opCtx, cmd.getNamespace(), cmd, nullptr, result);
    }

    if (cmd.getCollModRequest().getPrepareConstraintValidationLevel()) {
        return Status(ErrorCodes::InvalidOptions,
                      "option not supported on a time-series collection: "
                      "prepareConstraintValidationLevel");
    }

    if (isLegacyTimeseries) {
        // If there the expected collection UUID is provided, always fail because the user-facing
        // time-series doesn't have a UUID.
        checkCollectionUUIDMismatch(opCtx, cmd.getNamespace(), nullptr, cmd.getCollectionUUID());
    }

    // Aliasing collMod on a time-series collection in this manner has a few advantages:
    // - It supports modifying the expireAfterSeconds setting (which is also a collection creation
    //   option).
    // - It avoids any accidental changes to critical view-specific properties of thetime-series
    //   collection, which are important for maintaining the view-bucket relationship.
    auto timeseriesBucketsCmd =
        makeTimeseriesBucketsCollModCommand(*timeseriesOptions, cmd, isLegacyTimeseries);

    if (isLegacyTimeseries) {
        // We additionally create a special, limited collMod command for the view definition itself
        // if the pipeline needs to be updated to reflect changed timeseries options. This operation
        // is completed first. In the case that we get a partial update where only one of the two
        // collMod operations fully completes (e.g. replication rollback), having the view pipeline
        // update without updating the timeseries options on the buckets collection will result in
        // sub-optimal performance, but correct behavior. If the timeseries options were updated
        // without updating the view pipeline, we could end up with incorrect query behavior (namely
        // data missing from some queries).
        auto timeseriesViewCmd = makeTimeseriesViewCollModCommand(*timeseriesOptions, cmd);
        if (timeseriesViewCmd && performViewChange) {
            auto status = processCollModCommand(opCtx, nss, *timeseriesViewCmd, nullptr, result);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    return processCollModCommand(
        opCtx, timeseriesBucketsCmd->getNamespace(), *timeseriesBucketsCmd, nullptr, result);
}

}  // namespace timeseries
}  // namespace mongo
