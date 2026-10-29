// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo::query_settings::query_framework {
using namespace std::literals::string_view_literals;

constexpr std::string_view kClassic = "classic"sv;
constexpr std::string_view kSbe = "sbe"sv;

/**
 * Serializes the internal `QueryFrameworkControlEnum` values to the appropiate
 * 'querySettings.queryFramework' user-facing strings.
 */
std::string serialize(QueryFrameworkControlEnum queryFramework);

/**
 * Parses the 'querySettings.queryFramework' user-facing strings as 'QueryFrameworkControlEnum'
 * values.
 */
QueryFrameworkControlEnum parse(std::string_view queryFrameworkString);
}  // namespace mongo::query_settings::query_framework
