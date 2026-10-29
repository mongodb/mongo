// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/test_commands_enabled.h"

#include "mongo/db/commands/test_commands_enabled_gen.h"
#include "mongo/logv2/log_severity.h"

namespace mongo {

bool getTestCommandsEnabled() {
    return gEnableTestCommands;
}

void setTestCommandsEnabled(bool b) {
    // Enabling test commands suppresses the `LogSeverity::ProdOnly` level as a side effect.
    logv2::LogSeverity::suppressProdOnly_forTest(b);
    gEnableTestCommands = b;
}

Status onUpdateTestCommandsEnabled(bool b) {
    // Enabling test commands suppresses the `LogSeverity::ProdOnly` level as a side effect.
    logv2::LogSeverity::suppressProdOnly_forTest(b);
    return Status::OK();
}

}  // namespace mongo
