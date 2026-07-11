// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_format.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo::logv2 {

class LogDomain;
class LogDomainGlobal;
class LogComponentSettings;

/**
 * Container for managing log domains.
 *
 * Use this while setting up the logging system, before launching any threads.
 */
class [[MONGO_MOD_PUBLIC]] LogManager {
public:
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    LogManager();
    ~LogManager();

    static LogManager& global();

    /**
     * Gets the global domain for this manager.
     */
    LogDomain& getGlobalDomain();
    /**
     * Gets the internal global domain with an extended interface. Require the user to include
     * log_domain_global.h.
     */
    LogDomainGlobal& getGlobalDomainInternal();

    /**
     * Gets the global settings belonging to the global domain.
     */
    LogComponentSettings& getGlobalSettings();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo::logv2
