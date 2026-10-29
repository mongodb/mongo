// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/mongo_uri.h"
#include "mongo/shell/shell_options.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"

#include <cstring>
#include <string>
#include <string_view>

mongo::ShellGlobalParams mongo::shellGlobalParams;

void mongo::redactPasswordOptions(int argc, char** argv) {
    cmdline_utils::censorArgvArray(argc, argv);

    for (int i = 0; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (MongoURI::isMongoURI(arg)) {
            auto reformedURI = MongoURI::redact(arg);
            ::strncpy(argv[i], reformedURI.data(), arg.size());
        }
    }
}
