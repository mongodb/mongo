// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_context.h"

#include "mongo/db/query/query_lifespan.h"
#include "mongo/util/assert_util.h"

#include <variant>

namespace mongo::query_settings::query_settings_details {

namespace {
auto decoration = QueryLifespan::declareOpCtxDecoration<QuerySettingsOperationState>();
}  // namespace

QuerySettingsOperationState& getQuerySettingsStateForOp(OperationContext* opCtx) {
    return decoration(opCtx);
}
}  // namespace mongo::query_settings::query_settings_details
