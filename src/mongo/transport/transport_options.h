// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"

namespace mongo::transport {
Status onUpdateEstablishmentRefreshRate(int32_t newValue);

Status onUpdateEstablishmentBurstCapacitySecs(double newValue);

Status onUpdateEstablishmentMaxQueueDepth(int32_t newValue);
}  // namespace mongo::transport
