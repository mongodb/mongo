// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ServiceContext;

/**
 * Start the watchdog.
 */
void startWatchdog(ServiceContext* service);

/**
 * Callbacks used by the 'watchdogPeriodSeconds' set parameter.
 */
Status validateWatchdogPeriodSeconds(const int& value);
Status onUpdateWatchdogPeriodSeconds(const int& value);

}  // namespace mongo
