/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/timeseries/timeseries_collmod.h"

#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace timeseries {

std::unique_ptr<CollMod> makeTimeseriesBucketsCollModCommand(OperationContext* opCtx,
                                                             const CollMod& origCmd) {
    const auto& origNs = origCmd.getNamespace();

    auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, origNs, true);

    // Return early with null if we are not working with a time-series collection.
    if (!timeseriesOptions) {
        return {};
    }

    auto index = origCmd.getIndex();
    if (index && index->getKeyPattern()) {
        auto bucketsIndexSpecWithStatus = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
            *timeseriesOptions, *index->getKeyPattern());

        uassert(ErrorCodes::IndexNotFound,
                str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                              << " Command request: " << redact(origCmd.toBSON({})),
                bucketsIndexSpecWithStatus.isOK());

        index->setKeyPattern(std::move(bucketsIndexSpecWithStatus.getValue()));
    }

    auto ns = origNs.makeTimeseriesBucketsNamespace();
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
    request.setDryRun(origCmd.getDryRun());
    auto cmd = std::make_unique<CollMod>(ns);
    cmd->setCollModRequest(request);
    return cmd;
}

std::unique_ptr<CollMod> makeTimeseriesViewCollModCommand(OperationContext* opCtx,
                                                          const CollMod& origCmd) {
    const auto& ns = origCmd.getNamespace();

    auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, ns, true);

    // Return early with null if we are not working with a time-series collection.
    if (!timeseriesOptions) {
        return {};
    }

    auto& tsMod = origCmd.getTimeseries();
    if (tsMod) {
        auto res = timeseries::applyTimeseriesOptionsModifications(*timeseriesOptions, *tsMod);
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
    const auto* mainCmd = &cmd;
    // If the target namespace refers to a time-series collection, we will redirect the
    // collection modification request to the underlying bucket collection.
    // Aliasing collMod on a time-series collection in this manner has a few advantages:
    // - It supports modifying the expireAfterSeconds setting (which is also a collection
    //   creation option).
    // - It avoids any accidental changes to critical view-specific properties of the
    //   time-series collection, which are important for maintaining the view-bucket
    //   relationship.
    //
    // 'timeseriesBucketsCmd' is null if the request namespace does not refer to a time-series
    // collection. Otherwise, transforms the user time-series collMod request to one on the
    // underlying bucket collection.
    auto timeseriesBucketsCmd = makeTimeseriesBucketsCollModCommand(opCtx, cmd);
    if (timeseriesBucketsCmd) {
        // We additionally create a special, limited collMod command for the view definition
        // itself if the pipeline needs to be updated to reflect changed timeseries options.
        // This operation is completed first. In the case that we get a partial update where
        // only one of the two collMod operations fully completes (e.g. replication rollback),
        // having the view pipeline update without updating the timeseries options on the
        // buckets collection will result in sub-optimal performance, but correct behavior.
        // If the timeseries options were updated without updating the view pipeline, we could
        // end up with incorrect query behavior (namely data missing from some queries).
        auto timeseriesViewCmd = makeTimeseriesViewCollModCommand(opCtx, cmd);
        if (timeseriesViewCmd && performViewChange) {
            auto status = processCollModCommand(opCtx, nss, *timeseriesViewCmd, result);
            if (!status.isOK()) {
                return status;
            }
        }
        mainCmd = timeseriesBucketsCmd.get();
    }
    return processCollModCommand(opCtx, mainCmd->getNamespace(), *mainCmd, result);
}

}  // namespace timeseries
}  // namespace mongo
