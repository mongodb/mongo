// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Start Full Time Data Capture
 * Starts 1 thread.
 */
[[MONGO_MOD_PUBLIC]] void startMongoDFTDC(ServiceContext* serviceContext);

/**
 * Stop Full Time Data Capture
 */
[[MONGO_MOD_PUBLIC]] void stopMongoDFTDC();

/**
 * Validation callback for setParameter
 */
[[MONGO_MOD_PUBLIC]] Status validateCollectionStatsNamespaces(
    std::vector<std::string> value, const boost::optional<TenantId>& tenantId);

}  // namespace mongo
