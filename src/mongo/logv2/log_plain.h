// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>


namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] logv2 {

/**
 * This is used for dev stacktraces which bypass structured logging. LOGV2 sinks
 * are by default configured to use the JSONFormatter. We need use PlainFormatter
 * to output the message.
 *
 * This function is only expected to be used when the bazel flag `dev_stacktrace`
 * is enabled.
 */
void plainLogBypass(std::string_view message);

}  // namespace logv2
}  // namespace mongo
