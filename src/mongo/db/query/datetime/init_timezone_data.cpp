// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>

#include <timelib.h>

namespace mongo {
namespace {
ServiceContext::ConstructorActionRegisterer loadTimeZoneDB{
    "LoadTimeZoneDB", [](ServiceContext* service) {
        if (!serverGlobalParams.timeZoneInfoPath.empty()) {
            std::unique_ptr<timelib_tzdb, TimeZoneDatabase::TimeZoneDBDeleter> timeZoneDatabase(
                timelib_zoneinfo(const_cast<char*>(serverGlobalParams.timeZoneInfoPath.c_str())),
                TimeZoneDatabase::TimeZoneDBDeleter());
            if (!timeZoneDatabase) {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "failed to load time zone database from path \""
                                        << serverGlobalParams.timeZoneInfoPath << "\"");
            }
            TimeZoneDatabase::set(service,
                                  std::make_unique<TimeZoneDatabase>(std::move(timeZoneDatabase)));
        } else {
            // No 'zoneInfo' specified on the command line, fall back to the built-in rules.
            TimeZoneDatabase::set(service, std::make_unique<TimeZoneDatabase>());
        }
    }};
}  // namespace
}  // namespace mongo
