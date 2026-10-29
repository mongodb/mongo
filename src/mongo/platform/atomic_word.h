// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// This is a shim header to ease transition to new name. Prefer the new name in new code.
// The new name is "Atomic." The old name is "AtomicWord."
#include "mongo/platform/atomic.h"  // IWYU pragma: export
