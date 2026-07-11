// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <iosfwd>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ServiceContext;

/**
 * Writes useful information about the running process.
 * If `os` is nonnull, print to it, else to LOGV2.
 */
void logProcessDetails(std::ostream* os);

/**
 * Writes useful information about the running process to diagnostic log
 * for after a log rotation.
 */
void logProcessDetailsForLogRotate(ServiceContext* serviceContext);

}  // namespace mongo
