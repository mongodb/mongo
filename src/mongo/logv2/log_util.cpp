// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/logv2/log_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kControl


namespace mongo::logv2 {

namespace {
Atomic<bool> redactionEnabled{false};
Atomic<bool> redactBinDataEncrypt{true};
std::map<std::string_view, LogRotateCallback> logRotateCallbacks;
}  // namespace

void addLogRotator(std::string_view logType, LogRotateCallback cb) {
    logRotateCallbacks.emplace(logType, std::move(cb));
}

void LogRotateErrorAppender::append(const Status& err) {
    if (_combined.isOK()) {
        _combined = err;
    } else if (!err.isOK()) {
        // if there are multiple, distinct error codes, use OperationFailed instead
        auto newCode = (_combined.code() == err.code()) ? err.code() : ErrorCodes::OperationFailed;
        _combined = Status(newCode, str::stream() << _combined.reason() << ", " << err.reason());
    }
}

Status rotateLogs(bool renameFiles,
                  boost::optional<std::string_view> logType,
                  std::function<void(Status)> onMinorError) {
    std::string suffix = "." + terseCurrentTimeForFilename();

    LOGV2(23166, "Log rotation initiated", "suffix"_attr = suffix, "logType"_attr = logType);

    if (logType) {
        auto it = logRotateCallbacks.find(logType.value());
        if (it == logRotateCallbacks.end()) {
            LOGV2_WARNING(6221500, "Unknown log type for rotate", "logType"_attr = logType);
            return Status(ErrorCodes::NoSuchKey, "Unknown log type for rotate");
        }
        auto status = it->second(renameFiles, suffix, onMinorError);
        if (!status.isOK()) {
            LOGV2_WARNING(
                1947001, "Log rotation failed", "reason"_attr = status, "logType"_attr = logType);
        }
        return status;
    } else {
        LogRotateErrorAppender errors;
        for (const auto& entry : logRotateCallbacks) {
            auto status = entry.second(renameFiles, suffix, onMinorError);
            if (!status.isOK()) {
                LOGV2_WARNING(23168,
                              "Log rotation failed",
                              "reason"_attr = status,
                              "logType"_attr = entry.first);
                errors.append(status);
            }
        }
        return errors.getCombinedStatus();
    }
}

bool shouldRedactLogs() {
    return redactionEnabled.loadRelaxed();
}

void setShouldRedactLogs(bool enabled) {
    redactionEnabled.store(enabled);
}

bool shouldRedactBinDataEncrypt() {
    return redactBinDataEncrypt.loadRelaxed();
}

void setShouldRedactBinDataEncrypt(bool enabled) {
    redactBinDataEncrypt.store(enabled);
}

/** Stores the callback that is used to determine whether log service should be emitted. */
ShouldEmitLogServiceFn& emitLogServiceEnabled() {
    static StaticImmortal<ShouldEmitLogServiceFn> f{};
    return *f;
}

bool shouldEmitLogService() {
    auto fn = emitLogServiceEnabled();
    return fn && fn();
}

void setShouldEmitLogService(ShouldEmitLogServiceFn enabled) {
    invariant(enabled);
    emitLogServiceEnabled() = enabled;
}

}  // namespace mongo::logv2
