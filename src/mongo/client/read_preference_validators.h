// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"


namespace mongo {

[[MONGO_MOD_PUBLIC]] Status validateMaxStalenessSecondsExternal(
    std::int64_t maxStalenessSeconds, const boost::optional<TenantId>& tenantId = boost::none);

}  // namespace mongo
