// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {
using namespace std::literals::string_view_literals;

// These are hard-coded constants in the bucket schema.
static constexpr std::string_view kBucketIdFieldName = "_id"sv;
static constexpr std::string_view kBucketDataFieldName = "data"sv;
static constexpr std::string_view kBucketMetaFieldName = "meta"sv;
static constexpr std::string_view kBucketControlClosedFieldName = "closed"sv;
static constexpr std::string_view kBucketControlFieldName = "control"sv;
static constexpr std::string_view kBucketControlVersionFieldName = "version"sv;
static constexpr std::string_view kBucketControlCountFieldName = "count"sv;
static constexpr std::string_view kBucketControlMinFieldName = "min"sv;
static constexpr std::string_view kBucketControlMaxFieldName = "max"sv;
static constexpr std::string_view kControlMaxFieldNamePrefix = "control.max."sv;
static constexpr std::string_view kControlMinFieldNamePrefix = "control.min."sv;
static constexpr std::string_view kDataFieldNamePrefix = "data."sv;
static constexpr std::string_view kControlFieldNamePrefix = "control."sv;

// These are hard-coded field names in create collection for time-series collections.
static constexpr std::string_view kTimeFieldName = "timeField"sv;
static constexpr std::string_view kMetaFieldName = "metaField"sv;

// These are hard-coded field names in index specs.
static constexpr std::string_view kKeyFieldName = "key"sv;
static constexpr std::string_view kOriginalSpecFieldName = "originalSpec"sv;
static constexpr std::string_view kPartialFilterExpressionFieldName = "partialFilterExpression"sv;

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
static constexpr std::string_view kControlVersionPath = "control.version"sv;
static constexpr std::string_view kControlClosedPath = "control.closed"sv;

// DocDiff: constexpr versions of doc_diff::kSubDiffSectionFieldPrefix + bucket field names.
static constexpr std::string_view kDataFieldNameDocDiff = "sdata"sv;
static constexpr std::string_view kControlFieldNameDocDiff = "scontrol"sv;
static constexpr std::string_view kMinFieldNameDocDiff = "smin"sv;
static constexpr std::string_view kMaxFieldNameDocDiff = "smax"sv;

// Error code used to signal $out that it is attempting to create a legacy timeseries temp
// collection when viewless timeseries is enabled. $out catches this and retries with the
// viewless namespace.
// TODO SERVER-118970 remove once 9.0 becomes last LTS and all timeseries collections will be
// viewless.
static constexpr int kLegacyTimeseriesTempCollectionCreationError = 11281600;

inline const StringDataSet kAllowedCollectionCreationOptions{
    CreateCommand::kStorageEngineFieldName,
    CreateCommand::kIndexOptionDefaultsFieldName,
    CreateCommand::kCollationFieldName,
    CreateCommand::kTimeseriesFieldName,
    CreateCommand::kExpireAfterSecondsFieldName,
    CreateCommand::kTempFieldName};

}  // namespace mongo::timeseries
