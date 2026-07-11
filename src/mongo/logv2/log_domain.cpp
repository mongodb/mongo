// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_domain.h"

#include "mongo/logv2/log_domain_internal.h"

#include <memory>
#include <utility>

namespace mongo::logv2 {

LogDomain::LogDomain(std::unique_ptr<LogDomain::Internal> internalDomain)
    : _internal(std::move(internalDomain)) {}
LogDomain::~LogDomain() = default;

}  // namespace mongo::logv2
