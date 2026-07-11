// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/cluster_auth_mode_option_gen.h"
#include "mongo/db/keyfile_option_gen.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_nongeneral_gen.h"
#include "mongo/s/mongos_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"

#include <iostream>
#include <string>
#include <vector>

namespace mongo {
MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongosOptions)(InitializerContext* context) {
    uassertStatusOK(addGeneralServerOptions(&moe::startupOptions));
    uassertStatusOK(addKeyfileServerOption(&moe::startupOptions));
    uassertStatusOK(addClusterAuthModeServerOption(&moe::startupOptions));
    uassertStatusOK(addNonGeneralServerOptions(&moe::startupOptions));
}

MONGO_INITIALIZER_GENERAL(MongosOptions,
                          ("BeginStartupOptionValidation", "AllFailPointsRegistered"),
                          ("EndStartupOptionValidation"))
(InitializerContext* context) {
    if (!handlePreValidationMongosOptions(moe::startupOptionsParsed, context->args())) {
        quickExit(ExitCode::clean);
    }
    // Run validation, but tell the Environment that we don't want it to be set as "valid",
    // since we may be making it invalid in the canonicalization process.
    uassertStatusOK(moe::startupOptionsParsed.validate(false /*setValid*/));
    uassertStatusOK(validateMongosOptions(moe::startupOptionsParsed));
    uassertStatusOK(canonicalizeMongosOptions(&moe::startupOptionsParsed));
    uassertStatusOK(moe::startupOptionsParsed.validate());
}

MONGO_INITIALIZER_GENERAL(CoreOptions_Store,
                          ("BeginStartupOptionStorage"),
                          ("EndStartupOptionStorage"))
(InitializerContext* context) {
    Status ret = storeMongosOptions(moe::startupOptionsParsed);
    if (!ret.isOK()) {
        std::cerr << ret.toString() << std::endl;
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(ExitCode::badOptions);
    }
}

}  // namespace mongo
