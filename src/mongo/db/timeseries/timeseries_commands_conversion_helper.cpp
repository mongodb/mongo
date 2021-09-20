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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

namespace mongo::timeseries {

namespace {
NamespaceString makeTimeseriesBucketsNamespace(const NamespaceString& nss) {
    return nss.isTimeseriesBucketsCollection() ? nss : nss.makeTimeseriesBucketsNamespace();
}
}  // namespace


BSONObj makeTimeseriesCommand(const BSONObj& origCmd,
                              const NamespaceString& ns,
                              const StringData nsFieldName,
                              boost::optional<StringData> appendTimeSeriesFlag) {
    // Translate time-series collection view namespace to bucket namespace.
    const auto bucketNs = ns.makeTimeseriesBucketsNamespace();
    BSONObjBuilder builder;
    for (const auto& entry : origCmd) {
        if (entry.fieldNameStringData() == nsFieldName) {
            builder.append(nsFieldName, bucketNs.coll());
        } else {
            builder.append(entry);
        }
    }

    if (appendTimeSeriesFlag) {
        builder.append(*appendTimeSeriesFlag, true);
    }
    return builder.obj();
}

CreateIndexesCommand makeTimeseriesCreateIndexesCommand(OperationContext* opCtx,
                                                        const CreateIndexesCommand& origCmd,
                                                        const TimeseriesOptions& options) {
    const auto& origNs = origCmd.getNamespace();
    const auto& origIndexes = origCmd.getIndexes();

    std::vector<mongo::BSONObj> indexes;
    for (const auto& origIndex : origIndexes) {
        BSONObjBuilder builder;
        for (const auto& elem : origIndex) {
            if (elem.fieldNameStringData() == IndexDescriptor::kPartialFilterExprFieldName) {
                uasserted(ErrorCodes::InvalidOptions,
                          "Partial indexes are not supported on time-series collections");
            }

            if (elem.fieldNameStringData() == IndexDescriptor::kExpireAfterSecondsFieldName) {
                uasserted(ErrorCodes::InvalidOptions,
                          "TTL indexes are not supported on time-series collections");
            }

            if (elem.fieldNameStringData() == NewIndexSpec::kKeyFieldName) {
                auto pluginName = IndexNames::findPluginName(elem.Obj());
                uassert(ErrorCodes::InvalidOptions,
                        "Text indexes are not supported on time-series collections",
                        pluginName != IndexNames::TEXT);

                auto bucketsIndexSpecWithStatus =
                    timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(options, elem.Obj());
                uassert(ErrorCodes::CannotCreateIndex,
                        str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                                      << " Command request: " << redact(origCmd.toBSON({})),
                        bucketsIndexSpecWithStatus.isOK());

                builder.append(NewIndexSpec::kKeyFieldName,
                               std::move(bucketsIndexSpecWithStatus.getValue()));
                continue;
            }
            builder.append(elem);
        }

        indexes.push_back(builder.obj());
    }

    auto ns = makeTimeseriesBucketsNamespace(origNs);
    auto cmd = CreateIndexesCommand(ns, std::move(indexes));
    cmd.setV(origCmd.getV());
    cmd.setIgnoreUnknownIndexOptions(origCmd.getIgnoreUnknownIndexOptions());
    cmd.setCommitQuorum(origCmd.getCommitQuorum());

    return cmd;
}

DropIndexes makeTimeseriesDropIndexesCommand(OperationContext* opCtx,
                                             const DropIndexes& origCmd,
                                             const TimeseriesOptions& options) {
    const auto& origNs = origCmd.getNamespace();
    auto ns = makeTimeseriesBucketsNamespace(origNs);

    const auto& origIndex = origCmd.getIndex();
    if (auto keyPtr = stdx::get_if<BSONObj>(&origIndex)) {
        auto bucketsIndexSpecWithStatus =
            timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(options, *keyPtr);

        uassert(ErrorCodes::IndexNotFound,
                str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                              << " Command request: " << redact(origCmd.toBSON({})),
                bucketsIndexSpecWithStatus.isOK());

        return DropIndexes(ns, std::move(bucketsIndexSpecWithStatus.getValue()));
    }

    return DropIndexes(ns, origIndex);
}

}  // namespace mongo::timeseries
