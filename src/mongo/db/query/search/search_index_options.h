// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * Search index configuration options
 */
struct SearchIndexParams {
    static Status onValidateHost(std::string_view str, const boost::optional<TenantId>&);
    std::string host = "";
    bool skipAuthToSearchIndexServer = false;
};

extern SearchIndexParams globalSearchIndexParams;

}  // namespace mongo
