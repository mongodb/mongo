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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"

#include <functional>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::logv2 {

constexpr auto kServerLogTag = "server"_sd;
constexpr auto kAuditLogTag = "audit"_sd;

using LogRotateCallback = std::function<Status(bool, StringData, std::function<void(Status)>)>;
using ShouldEmitLogServiceFn = std::function<bool()>;

/**
 * logType param needs to have static lifetime. If a new logType needs to be defined, add it above
 * with the other constexpr logTags.
 */
void addLogRotator(StringData logType, LogRotateCallback cb);

/**
 * Class that combines error Status objects into a single Status object.
 */
class LogRotateErrorAppender {
public:
    LogRotateErrorAppender() : _combined(Status::OK()) {}
    LogRotateErrorAppender(const Status& init) : _combined(init) {}

    const Status& getCombinedStatus() const {
        return _combined;
    }

    void append(const Status& err);

private:
    Status _combined;
};

/**
 * Rotates the log files.  Returns Status::OK() if all logs rotate successfully.
 *
 * renameFiles - true means we rename files, false means we expect the file to be renamed
 *               externally
 *
 * logrotate on *nix systems expects us not to rename the file, it is expected that the program
 * simply open the file again with the same name.
 * We expect logrotate to rename the existing file before we rotate, and so the next open
 * we do should result in a file create.
 */
Status rotateLogs(bool renameFiles,
                  boost::optional<StringData> logType,
                  std::function<void(Status)> onMinorError);

/**
 * Returns true if system logs should be redacted.
 */
bool shouldRedactLogs();

/**
 * Set the 'redact' mode of the server.
 */
void setShouldRedactLogs(bool enabled);

/**
 * Returns true if the BinData Encrypt should be redacted. Default true.
 */
bool shouldRedactBinDataEncrypt();

/**
 * Sets the redact mode of the bin data encrypt field.
 */
void setShouldRedactBinDataEncrypt(bool enabled);

/**
 * Returns true if log service names should be emitted. Returns false until setShouldEmitLogService
 * is called.
 */
bool shouldEmitLogService();

/**
 * Set a callback which shouldEmitLogService() invokes to determine whether log service names should
 * be emitted.
 */
void setShouldEmitLogService(ShouldEmitLogServiceFn fn);

}  // namespace mongo::logv2
