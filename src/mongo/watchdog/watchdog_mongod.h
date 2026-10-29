// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ServiceContext;

/**
 * Start the watchdog.
 */
void startWatchdog(ServiceContext* service);

/**
 * Callbacks used by the 'watchdogPeriodSeconds' set parameter.
 */
Status validateWatchdogPeriodSeconds(const int& value, const boost::optional<TenantId>&);
Status onUpdateWatchdogPeriodSeconds(const int& value);

}  // namespace mongo
