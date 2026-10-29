// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_service.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] logv2 {

class UserAssertAfterLog {
public:
    UserAssertAfterLog() : errorCode(constants::kUserAssertWithLogID) {}
    explicit UserAssertAfterLog(ErrorCodes::Error code) : errorCode(code) {}

    int32_t errorCode;
};

enum class FatalMode { kAssert, kAssertNoTrace, kContinue };

class LogOptions {
public:
    static LogOptions ensureValidComponent(LogOptions options, LogComponent component) {
        if (options._component == LogComponent::kAutomaticDetermination) {
            options._component = component;
        }
        if (options._service == LogService::defer) {
            options._service = getLogService();
        }
        return options;
    }

    LogOptions(LogComponent component) : _component(component) {}

    LogOptions(LogComponent component, FatalMode mode) : _component(component), _fatalMode(mode) {}

    LogOptions(LogComponent component, LogService service)
        : _component(component), _service(service) {}

    LogOptions(LogComponent component, LogService service, FatalMode mode)
        : _component(component), _service(service), _fatalMode(mode) {}

    LogOptions(LogService service) : _service(service) {}

    LogOptions(LogDomain* domain) : _domain(domain) {}

    LogOptions(LogTag tags) : _tags(tags) {}

    LogOptions(LogTruncation truncation) : _truncation(truncation) {}

    LogOptions(UserAssertAfterLog uassertAfterLog)
        : _userAssertErrorCode(uassertAfterLog.errorCode) {}

    LogOptions(FatalMode mode) : _fatalMode(mode) {}

    LogOptions(FatalMode mode, LogTruncation truncation)
        : _truncation(truncation), _fatalMode(mode) {}

    LogOptions(LogTag tags, LogTruncation truncation) : _tags(tags), _truncation(truncation) {}

    LogOptions(LogComponent component, LogDomain* domain, LogTag tags)
        : _domain(domain), _tags(tags), _component(component) {}

    LogOptions(LogTruncation truncation, UserAssertAfterLog uassertAfterLog)
        : _truncation(truncation), _userAssertErrorCode(uassertAfterLog.errorCode) {}

    LogOptions(LogComponent component,
               LogDomain* domain,
               LogTag tags,
               LogTruncation truncation,
               FatalMode fatalMode)
        : _domain(domain),
          _tags(tags),
          _component(component),
          _truncation(truncation),
          _fatalMode(fatalMode) {}

    LogComponent component() const {
        return _component;
    }

    LogDomain& domain() const {
        return *_domain;
    }

    LogTag tags() const {
        return _tags;
    }

    LogTruncation truncation() const {
        return _truncation;
    }

    int32_t uassertErrorCode() const {
        return _userAssertErrorCode;
    }

    FatalMode fatalMode() const {
        return _fatalMode;
    }

    LogService service() const {
        return _service;
    }

private:
    LogDomain* _domain = &LogManager::global().getGlobalDomain();
    LogTag _tags;
    LogComponent _component = LogComponent::kAutomaticDetermination;
    LogService _service = LogService::defer;
    LogTruncation _truncation = constants::kDefaultTruncation;
    int32_t _userAssertErrorCode = ErrorCodes::OK;
    FatalMode _fatalMode = FatalMode::kAssert;
};

}  // namespace logv2
}  // namespace mongo
