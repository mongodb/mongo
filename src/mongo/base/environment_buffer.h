// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {
// Instead of making a forward declaration to `extern char** environ;`, it's better to call this
// function.  The way that `environ` is linked into a final binary is different on different
// UNIX-like platforms and can cause issues with our link-graph verification.  Calling this function
// and linking to this library resolve those issues correctly for our codebase.
char** getEnvironPointer();
}  // namespace mongo
