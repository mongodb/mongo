// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns a TailableMode from two booleans, returning ErrorCodes::FailedToParse if awaitData is
 * set without tailable.
 */
StatusWith<TailableModeEnum> tailableModeFromBools(bool isTailable, bool isAwaitData);

}  // namespace mongo
