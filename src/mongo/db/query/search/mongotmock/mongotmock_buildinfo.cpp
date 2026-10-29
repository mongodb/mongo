// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/buildinfo_common.h"

namespace mongo {
namespace {

/**
 * Implements { buildInfo : 1} for mock mongocryptd.
 */
class MongotMockBuildInfo : public CmdBuildInfoCommon {
public:
    using CmdBuildInfoCommon::CmdBuildInfoCommon;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        MONGO_UNREACHABLE;
    }

    BuildInfo generateBuildInfo(OperationContext* opCtx) const final {
        auto reply = CmdBuildInfoCommon::generateBuildInfo(opCtx);
        reply.setMongotmock(true);
        return reply;
    }
};
MONGO_REGISTER_COMMAND(MongotMockBuildInfo).forShard();

}  // namespace
}  // namespace mongo
