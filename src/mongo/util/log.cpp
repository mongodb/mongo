/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl
#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kControl


#include "mongo/platform/basic.h"

#include "mongo/logv2/log.h"
#include "mongo/util/log.h"

#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logger/ramlog.h"
#include "mongo/logger/rotatable_file_manager.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/time_support.h"

// TODO: Win32 unicode console writing (in logger/console_appender?).
// TODO: Extra log context appending, and re-enable log_user_*.js
// TODO: Eliminate cout/cerr.

namespace mongo {

#if defined(MONGO_CONFIG_LOGV1_DEFAULT)
static bool _logV2Enabled = false;
#else
static bool _logV2Enabled = true;
#endif

bool logV2Enabled() {
    return _logV2Enabled;
}

void logV2Set(bool setting) {
    _logV2Enabled = setting;
}

static logger::ExtraLogContextFn _appendExtraLogContext;

Status logger::registerExtraLogContextFn(logger::ExtraLogContextFn contextFn) {
    if (!contextFn)
        return Status(ErrorCodes::BadValue, "Cannot register a NULL log context function.");
    if (_appendExtraLogContext) {
        return Status(ErrorCodes::AlreadyInitialized,
                      "Cannot call registerExtraLogContextFn multiple times.");
    }
    _appendExtraLogContext = contextFn;
    return Status::OK();
}

bool rotateLogs(bool renameFiles) {
    // Rotate on both logv1 and logv2 so all files that need rotation gets rotated
    LOGV2(23166, "Log rotation initiated");
    std::string suffix = "." + terseCurrentTime(false);
    Status resultv2 =
        logv2::LogManager::global().getGlobalDomainInternal().rotate(renameFiles, suffix);
    if (!resultv2.isOK())
        LOGV2_WARNING(23168, "Log rotation failed: {resultv2}", "resultv2"_attr = resultv2);

    using logger::RotatableFileManager;
    RotatableFileManager* manager = logger::globalRotatableFileManager();
    RotatableFileManager::FileNameStatusPairVector result(manager->rotateAll(renameFiles, suffix));
    for (RotatableFileManager::FileNameStatusPairVector::iterator it = result.begin();
         it != result.end();
         it++) {
        LOGV2_WARNING(23169,
                      "Rotating log file {it_first} failed: {it_second}",
                      "it_first"_attr = it->first,
                      "it_second"_attr = it->second.toString());
    }
    return resultv2.isOK() && result.empty();
}

void logContext(const char* errmsg) {
    if (errmsg) {
        LOGV2(23167, "{errmsg}", "errmsg"_attr = errmsg);
    }
    // NOTE: We disable long-line truncation for the stack trace, because the JSON representation of
    // the stack trace can sometimes exceed the long line limit.
    printStackTrace(log().setIsTruncatable(false).stream());
}

void setPlainConsoleLogger() {
    logger::globalLogManager()->getGlobalDomain()->clearAppenders();
    logger::globalLogManager()->getGlobalDomain()->attachAppender(
        std::make_unique<logger::ConsoleAppender<logger::MessageEventEphemeral>>(
            std::make_unique<logger::MessageEventUnadornedEncoder>()));
}

Tee* const startupWarningsLog = RamLog::get("startupWarnings");  // intentionally leaked

}  // namespace mongo
