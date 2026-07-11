// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/cluster_auth_mode_option_gen.h"
#include "mongo/db/keyfile_option_gen.h"
#include "mongo/db/server_options_nongeneral_gen.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {
namespace {

MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongotMockOptions)(InitializerContext* context) {
    uassertStatusOK(addNonGeneralServerOptions(&optionenvironment::startupOptions));
    uassertStatusOK(addKeyfileServerOption(&optionenvironment::startupOptions));
    uassertStatusOK(addClusterAuthModeServerOption(&optionenvironment::startupOptions));
}

}  // namespace
}  // namespace mongo
