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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace timeseries {

// These are hard-coded constants in the bucket schema.
static constexpr StringData kBucketIdFieldName = "_id"_sd;
static constexpr StringData kBucketDataFieldName = "data"_sd;
static constexpr StringData kBucketMetaFieldName = "meta"_sd;
static constexpr StringData kBucketControlClosedFieldName = "closed"_sd;
static constexpr StringData kBucketControlFieldName = "control"_sd;
static constexpr StringData kBucketControlVersionFieldName = "version"_sd;
static constexpr StringData kBucketControlCountFieldName = "count"_sd;
static constexpr StringData kBucketControlMinFieldName = "min"_sd;
static constexpr StringData kBucketControlMaxFieldName = "max"_sd;
static constexpr StringData kControlMaxFieldNamePrefix = "control.max."_sd;
static constexpr StringData kControlMinFieldNamePrefix = "control.min."_sd;
static constexpr StringData kDataFieldNamePrefix = "data."_sd;
static constexpr StringData kControlFieldNamePrefix = "control."_sd;

// These are hard-coded field names in create collection for time-series collections.
static constexpr StringData kTimeFieldName = "timeField"_sd;
static constexpr StringData kMetaFieldName = "metaField"_sd;

// These are hard-coded field names in index specs.
static constexpr StringData kKeyFieldName = "key"_sd;
static constexpr StringData kOriginalSpecFieldName = "originalSpec"_sd;
static constexpr StringData kPartialFilterExpressionFieldName = "partialFilterExpression"_sd;

// There are 3 versions of buckets. The first is uncompressed, the second is compressed
// and has its records sorted on time, and the third is compressed and does not have its
// records sorted on time.
static constexpr int kTimeseriesControlUncompressedVersion = 1;
static constexpr int kTimeseriesControlCompressedSortedVersion = 2;
static constexpr int kTimeseriesControlCompressedUnsortedVersion = 3;

// This is the latest version that we default to. Therefore, even though v3 > v2,
// since by default we will still create v2 buckets and only promote them to v3
// if they receive an out-of-order insert, this remains at v2.
static constexpr int kTimeseriesControlLatestVersion = kTimeseriesControlCompressedSortedVersion;
static constexpr int kTimeseriesControlMinVersion = kTimeseriesControlUncompressedVersion;

// These are hard-coded control object subfields.
static constexpr StringData kControlVersionPath = "control.version"_sd;
static constexpr StringData kControlClosedPath = "control.closed"_sd;

// DocDiff: constexpr versions of doc_diff::kSubDiffSectionFieldPrefix + bucket field names.
static constexpr StringData kDataFieldNameDocDiff = "sdata"_sd;
static constexpr StringData kControlFieldNameDocDiff = "scontrol"_sd;
static constexpr StringData kMinFieldNameDocDiff = "smin"_sd;
static constexpr StringData kMaxFieldNameDocDiff = "smax"_sd;

static const StringDataSet kAllowedCollectionCreationOptions{
    CreateCommand::kStorageEngineFieldName,
    CreateCommand::kIndexOptionDefaultsFieldName,
    CreateCommand::kCollationFieldName,
    CreateCommand::kTimeseriesFieldName,
    CreateCommand::kExpireAfterSecondsFieldName,
    CreateCommand::kTempFieldName};

}  // namespace timeseries
}  // namespace mongo
