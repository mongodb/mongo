// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/basic_types.h"
#include "mongo/db/commands.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
using namespace std::literals::string_view_literals;
constexpr inline auto kDefaultMaxTimeMSClusterParameterName = "defaultMaxTimeMS"sv;

/**
 * Returns the value of maxTimeMS that should be used for a command or boost::none if none could be
 * used. Also returns if 'defaultMaxTimeMS' was used.
 */
std::pair<boost::optional<Milliseconds>, bool> getRequestOrDefaultMaxTimeMS(
    OperationContext* opCtx, boost::optional<std::int64_t> requestMaxTimeMS, bool isReadOperation);
}  // namespace mongo
