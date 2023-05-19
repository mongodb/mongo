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

#include "mongo/platform/basic.h"

#include "mongo/db/query/stage_types.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
StringData stageTypeToString(StageType stageType) {
    static const stdx::unordered_map<StageType, StringData> kStageTypesMap = {
        {STAGE_AND_HASH, "AND_HASH"_sd},
        {STAGE_AND_SORTED, "AND_SORTED"_sd},
        {STAGE_BATCHED_DELETE, "BATCHED_DELETE"_sd},
        {STAGE_CACHED_PLAN, "CACHED_PLAN"},
        {STAGE_COLLSCAN, "COLLSCAN"_sd},
        {STAGE_COLUMN_SCAN, "COLUMN_SCAN"_sd},
        {STAGE_COUNT, "COUNT"_sd},
        {STAGE_COUNT_SCAN, "COUNT_SCAN"_sd},
        {STAGE_DELETE, "DELETE"_sd},
        {STAGE_DISTINCT_SCAN, "DISTINCT_SCAN"_sd},
        {STAGE_EOF, "EOF"_sd},
        {STAGE_EQ_LOOKUP, "EQ_LOOKUP"_sd},
        {STAGE_FETCH, "FETCH"_sd},
        {STAGE_GEO_NEAR_2D, "GEO_NEAR_2D"_sd},
        {STAGE_GEO_NEAR_2DSPHERE, "GEO_NEAR_2DSPHERE"_sd},
        {STAGE_GROUP, "GROUP"_sd},
        {STAGE_IDHACK, "IDHACK"_sd},
        {STAGE_IXSCAN, "IXSCAN"_sd},
        {STAGE_LIMIT, "LIMIT"_sd},
        {STAGE_MOCK, "MOCK"_sd},
        {STAGE_MULTI_ITERATOR, "MULTI_ITERATOR"_sd},
        {STAGE_MULTI_PLAN, "MULTI_PLAN"_sd},
        {STAGE_OR, "OR"_sd},
        {STAGE_PROJECTION_DEFAULT, "PROJECTION_DEFAULT"_sd},
        {STAGE_PROJECTION_COVERED, "PROJECTION_COVERED"_sd},
        {STAGE_PROJECTION_SIMPLE, "PROJECTION_SIMPLE"_sd},
        {STAGE_QUEUED_DATA, "QUEUED_DATA"_sd},
        {STAGE_RECORD_STORE_FAST_COUNT, "RECORD_STORE_FAST_COUNT"_sd},
        {STAGE_RETURN_KEY, "RETURN_KEY"_sd},
        {STAGE_SAMPLE_FROM_TIMESERIES_BUCKET, "SAMPLE_FROM_TIMESERIES_BUCKET"_sd},
        {STAGE_SHARDING_FILTER, "SHARDING_FILTER"_sd},
        {STAGE_SKIP, "SKIP"_sd},
        {STAGE_SORT_DEFAULT, "SORT"_sd},
        {STAGE_SORT_SIMPLE, "SORT"_sd},
        {STAGE_SORT_KEY_GENERATOR, "SORT_KEY_GENERATOR"_sd},
        {STAGE_SORT_MERGE, "SORT_MERGE"_sd},
        {STAGE_SPOOL, "SPOOL"_sd},
        {STAGE_SUBPLAN, "SUBPLAN"_sd},
        {STAGE_TEXT_OR, "TEXT_OR"_sd},
        {STAGE_TEXT_MATCH, "TEXT_MATCH"_sd},
        {STAGE_TIMESERIES_MODIFY, "TIMESERIES_MODIFY"_sd},
        {STAGE_TRIAL, "TRIAL"_sd},
        {STAGE_UNKNOWN, "UNKNOWN"_sd},
        {STAGE_UNPACK_TIMESERIES_BUCKET, "UNPACK_TIMESERIES_BUCKET"_sd},
        {STAGE_UPDATE, "UPDATE"_sd},
    };
    if (auto it = kStageTypesMap.find(stageType); it != kStageTypesMap.end()) {
        return it->second;
    }
    return kStageTypesMap.at(STAGE_UNKNOWN);
}

StringData sbeClusteredCollectionScanToString() {
    static const StringData kClusteredIxscan = "CLUSTERED_IXSCAN"_sd;
    return kClusteredIxscan;
}
}  // namespace mongo
