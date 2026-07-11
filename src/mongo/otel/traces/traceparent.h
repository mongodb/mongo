// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::otel::traces {

/**
 * Validates that `value` is a well-formed W3C `traceparent` value of the form
 * "<version>-<trace-id>-<parent-id>-<trace-flags>" as defined by
 * https://www.w3.org/TR/trace-context/#traceparent-header.
 */
[[MONGO_MOD_PUBLIC]] Status validateW3CTraceparent(std::string_view value);

}  // namespace mongo::otel::traces
