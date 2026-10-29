// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/shell_utils.h"

namespace mongo {

namespace JSFiles {
extern const JSFile keyvault;
}

namespace {

void callback_fn(Scope& scope) {
    scope.execSetup(JSFiles::keyvault);
}

MONGO_INITIALIZER(setKeyvaultCallback)(InitializerContext*) {
    shell_utils::setEnterpriseShellCallback(mongo::callback_fn);
}

}  // namespace
}  // namespace mongo
