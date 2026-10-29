// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {

MONGO_INITIALIZER_GENERAL(ServerOptions_Setup,
                          ("BeginStartupOptionSetup"),
                          ("EndStartupOptionSetup"))
(InitializerContext* context) {
    uassertStatusOK(setupServerOptions(context->args()));
}

}  // namespace mongo
