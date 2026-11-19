// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/command_execution_context_handle.h"

namespace mongo::extension::sdk {

ExtensionGenericStatus CommandExecutionContextAPI::checkForInterrupt() const {
    ExtensionGenericStatus opStatus;
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().check_for_interrupt(get(), &opStatus); });
    return opStatus;
}

int64_t CommandExecutionContextAPI::getDeadlineTimestampMs() const {
    int64_t deadlineTimestampMs{0};
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_deadline_timestamp_ms(get(), &deadlineTimestampMs); });
    return deadlineTimestampMs;
}

}  // namespace mongo::extension::sdk
