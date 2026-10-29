// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Sets the appropriate state to reflect the value of the `enableDiagnosticLogging` knob.
 */
Status onUpdateEnableDiagnosticLogging(bool newValue);

/**
 * Sets the appropriate state to reflect the value of the
 * `signalHandlerUsesDiagnosticLogging` knob.
 */
Status onUpdateSignalHandlerUsesDiagnosticLogging(bool newValue);
}  // namespace mongo
