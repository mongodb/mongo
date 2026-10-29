// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/assert_util_parameters.h"

#include "mongo/base/initializer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/assert_util_parameters_gen.h"
#include "mongo/util/signal_handlers_synchronous.h"

namespace mongo {

MONGO_INITIALIZER(SetUpDiagnosticLoggingStatus)(InitializerContext*) {
    bool startUpVal = enableDiagnosticLogging.load();
    bool signalHandlerLoggingVal = signalHandlerUsesDiagnosticLogging.load();
    setDiagnosticLoggingInSignalHandlers(startUpVal && signalHandlerLoggingVal);
    setDiagnosticLoggingInAssertUtil(startUpVal);
    setScopedDebugInfoStackEnabled(startUpVal);
}

Status onUpdateEnableDiagnosticLogging(bool newValue) {
    setDiagnosticLoggingInSignalHandlers(newValue);
    setDiagnosticLoggingInAssertUtil(newValue);
    setScopedDebugInfoStackEnabled(newValue);
    return Status::OK();
}

Status onUpdateSignalHandlerUsesDiagnosticLogging(bool newValue) {
    setDiagnosticLoggingInSignalHandlers(newValue);
    return Status::OK();
}

}  // namespace mongo
