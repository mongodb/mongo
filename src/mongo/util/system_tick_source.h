// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"

#include <memory>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/** Tick source based on `std::chrono::steady_clock`. Monotonic, cheap, and high-precision. */
std::unique_ptr<TickSource> makeSystemTickSource();

/** Accesses a singleton instance made by `makeSystemTickSource`. Safe to call at any time. */
TickSource* globalSystemTickSource();

}  // namespace mongo
