// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/init.h"

namespace mongo {
namespace {
// we need a no-op initializer so that we can depend on SSLManager as a prerequisite in
// non-SSL builds.
MONGO_INITIALIZER(SSLManager)(InitializerContext*) {}
}  // namespace
}  // namespace mongo
