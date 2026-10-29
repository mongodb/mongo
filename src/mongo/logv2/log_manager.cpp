// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_manager.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_domain_internal.h"
#include "mongo/logv2/log_util.h"

#include <functional>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo::logv2 {

struct LogManager::Impl {
    Impl() {}

    LogDomain _globalDomain{std::make_unique<LogDomainGlobal>()};
};


LogManager::LogManager() {
    _impl = std::make_unique<Impl>();
}

LogManager::~LogManager() {}

LogManager& LogManager::global() {
    static LogManager* globalLogManager = new LogManager();
    return *globalLogManager;
}

LogDomain& LogManager::getGlobalDomain() {
    return _impl->_globalDomain;
}

LogDomainGlobal& LogManager::getGlobalDomainInternal() {
    return static_cast<LogDomainGlobal&>(_impl->_globalDomain.internal());
}

LogComponentSettings& LogManager::getGlobalSettings() {
    return getGlobalDomainInternal().settings();
}

MONGO_INITIALIZER(GlobalLogRotator)(InitializerContext*) {
    addLogRotator(
        logv2::kServerLogTag,
        [](bool renameFiles, std::string_view suffix, std::function<void(Status)> onMinorError) {
            return LogManager::global().getGlobalDomainInternal().rotate(
                renameFiles, suffix, onMinorError);
        });
}

}  // namespace mongo::logv2
