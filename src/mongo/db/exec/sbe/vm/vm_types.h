// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>  // uint8_t, etc.

namespace mongo::sbe::vm {

/**
 * Common type aliases for SBE.
 */
using ArityType = uint32_t;
using LabelId = int64_t;
using SmallArityType = uint8_t;

}  // namespace mongo::sbe::vm
