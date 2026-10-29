// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace change_stream_constants {
namespace stage_names {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kEnsureResumeTokenPresent =
    "$_internalChangeStreamEnsureResumeTokenPresent"sv;
static constexpr std::string_view kHandleTopologyChange =
    "$_internalChangeStreamHandleTopologyChange"sv;
static constexpr std::string_view kHandleTopologyChangeV2 =
    "$_internalChangeStreamHandleTopologyChangeV2"sv;
}  // namespace stage_names

static const BSONObj kSortSpec = BSON("_id._data" << 1);

// Internal change stream stages that can appear in a router (mongoS) pipeline.
static const StringDataSet kChangeStreamRouterPipelineStages = {
    stage_names::kEnsureResumeTokenPresent,
    stage_names::kHandleTopologyChange,
    stage_names::kHandleTopologyChangeV2};

}  // namespace change_stream_constants
}  // namespace mongo
