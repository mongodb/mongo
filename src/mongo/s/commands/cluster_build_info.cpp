// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/commands.h"
#include "mongo/db/commands/buildinfo_common.h"

namespace mongo {
namespace {
MONGO_REGISTER_COMMAND(CmdBuildInfoCommon).forRouter();
}  // namespace
}  // namespace mongo
