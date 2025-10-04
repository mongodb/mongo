/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/logv2/log_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <map>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kControl


namespace mongo::logv2 {

namespace {
AtomicWord<bool> redactionEnabled{false};
AtomicWord<bool> redactBinDataEncrypt{true};
std::map<StringData, LogRotateCallback> logRotateCallbacks;
}  // namespace

void addLogRotator(StringData logType, LogRotateCallback cb) {
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
                  boost::optional<StringData> logType,
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
