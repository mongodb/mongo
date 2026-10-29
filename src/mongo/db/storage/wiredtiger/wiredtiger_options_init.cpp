// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

#include <iostream>
#include <vector>

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

namespace moe = mongo::optionenvironment;

namespace mongo {

MONGO_STARTUP_OPTIONS_STORE(WiredTigerOptions)(InitializerContext* context) {
    Status ret = wiredTigerGlobalOptions.store(moe::startupOptionsParsed);
    if (!ret.isOK()) {
        std::cerr << ret.toString() << std::endl;
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        ::_exit(static_cast<int>(ExitCode::badOptions));
    }
}
}  // namespace mongo
