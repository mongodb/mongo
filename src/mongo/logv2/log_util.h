// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <functional>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] logv2 {
using namespace std::literals::string_view_literals;

constexpr auto kServerLogTag = "server"sv;
constexpr auto kAuditLogTag = "audit"sv;

#ifdef _WIN32
constexpr auto kWindowsNUL = "nul"sv;
#endif

using LogRotateCallback =
    std::function<Status(bool, std::string_view, std::function<void(Status)>)>;
using ShouldEmitLogServiceFn = std::function<bool()>;

/**
 * logType param needs to have static lifetime. If a new logType needs to be defined, add it above
 * with the other constexpr logTags.
 */
void addLogRotator(std::string_view logType, LogRotateCallback cb);

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
                  boost::optional<std::string_view> logType,
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

#ifdef _WIN32
/**
 * Checks for the Windows NUL file. Although it's a valid file, boost::filesystem::exists() throws
 * an exception when checking it so we use this function as a special case.
 */
inline bool isLogPathWindowsNul(const std::string& path) {
    return str::equalCaseInsensitive(path, kWindowsNUL);
}
#endif

}  // namespace logv2
}  // namespace mongo
