// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ServiceContext;

/**
 * Returns a new PeriodicRunner.
 */
std::unique_ptr<PeriodicRunner> makePeriodicRunner(ServiceContext* svc);

}  // namespace mongo
