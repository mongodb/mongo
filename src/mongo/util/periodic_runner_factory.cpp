// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/periodic_runner_factory.h"

#include "mongo/db/service_context.h"
#include "mongo/util/periodic_runner_impl.h"

namespace mongo {

std::unique_ptr<PeriodicRunner> makePeriodicRunner(ServiceContext* svc) {
    return std::make_unique<PeriodicRunnerImpl>(svc, svc->getPreciseClockSource());
}

}  // namespace mongo
