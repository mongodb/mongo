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


#include <boost/date_time/posix_time/posix_time_types.hpp>
// IWYU pragma: no_include "boost/date_time/gregorian_calendar.ipp"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/balancer_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/date_time/time_duration.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/operators.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {
const std::array<std::string, 7> kDaysOfWeek = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Parses time of day in "hh:mm" format assuming 'hh' is 00-23.
bool toPointInTime(const std::string& str, boost::posix_time::ptime* timeOfDay) {
    int hh = 0;
    int mm = 0;
    if (2 != sscanf(str.c_str(), "%d:%d", &hh, &mm)) {
        return false;
    }

    // Verify that time is well formed.
    if ((hh / 24) || (mm / 60)) {
        return false;
    }

    boost::posix_time::ptime res(boost::posix_time::second_clock::local_time().date(),
                                 boost::posix_time::hours(hh) + boost::posix_time::minutes(mm));
    *timeOfDay = res;
    return true;
}

const char kValue[] = "value";
const char kEnabled[] = "enabled";
const char kStopped[] = "stopped";
const char kMode[] = "mode";
const char kActiveWindow[] = "activeWindow";
const char kWaitForDelete[] = "_waitForDelete";
const char kAttemptToBalanceJumboChunks[] = "attemptToBalanceJumboChunks";
const char kActiveWindowDOW[] = "activeWindowDOW";
const char kDay[] = "day";
const char kStart[] = "start";
const char kStop[] = "stop";

bool isValidDayOfWeek(const std::string& dayStr) {
    return std::find(kDaysOfWeek.begin(), kDaysOfWeek.end(), dayStr) != kDaysOfWeek.end();
}

std::string getDayOfWeekString(const boost::gregorian::date& date) {
    return kDaysOfWeek[date.day_of_week()];
}

}  // namespace

const std::vector<std::string> balancerModes = {"full", "off"};
const BSONObj BalancerSettingsType::kSchema =
    BSON("properties"
         << BSON("_id" << BSON("enum" << BSON_ARRAY(BalancerConfiguration::kBalancerSettingKey))
                       << kMode << BSON("enum" << balancerModes) << kStopped
                       << BSON("bsonType" << "bool") << kActiveWindow
                       << BSON("bsonType" << "object"
                                          << "required" << BSON_ARRAY("start" << "stop"))
                       << kActiveWindowDOW << BSON("bsonType" << "array") << "_secondaryThrottle"
                       << BSON("oneOf" << BSON_ARRAY(BSON("bsonType" << "bool")
                                                     << BSON("bsonType" << "object")))
                       << kWaitForDelete << BSON("bsonType" << "bool")
                       << kAttemptToBalanceJumboChunks << BSON("bsonType" << "bool"))
         << "additionalProperties" << false);

const char ChunkSizeSettingsType::kKey[] = "chunksize";
const uint64_t ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes{128 * 1024 * 1024};
const BSONObj ChunkSizeSettingsType::kSchema = BSON(
    "properties" << BSON("_id" << BSON("enum" << BSON_ARRAY(ChunkSizeSettingsType::kKey)) << kValue
                               << BSON("bsonType" << "number"
                                                  << "minimum" << 1 << "maximum" << 1024))
                 << "additionalProperties" << false);

const char AutoMergeSettingsType::kKey[] = "automerge";

BalancerConfiguration::BalancerConfiguration()
    : _balancerSettings(BalancerConfiguration::createDefaultSettings()),
      _maxChunkSizeBytes(ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes),
      _shouldAutoMerge(true) {}

BalancerConfiguration::~BalancerConfiguration() = default;

BalancerModeEnum BalancerConfiguration::getBalancerMode() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _balancerSettings.getMode();
}

Status BalancerConfiguration::setBalancerMode(OperationContext* opCtx, BalancerModeEnum mode) {
    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        NamespaceString::kConfigSettingsNamespace,
        BSON("_id" << kBalancerSettingKey),
        BSON("$set" << BSON(kStopped << (mode == BalancerModeEnum::kOff) << kMode
                                     << BalancerMode_serializer(mode))),
        true,
        defaultMajorityWriteConcernDoNotUse());

    Status refreshStatus = refreshAndCheck(opCtx);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    if (!updateStatus.isOK() && (getBalancerMode() != mode)) {
        return updateStatus.getStatus().withContext(str::stream()
                                                    << "Failed to set the balancer mode to "
                                                    << BalancerMode_serializer(mode));
    }

    return Status::OK();
}

Status BalancerConfiguration::changeAutoMergeSettings(OperationContext* opCtx, bool enable) {
    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        NamespaceString::kConfigSettingsNamespace,
        BSON("_id" << AutoMergeSettingsType::kKey),
        BSON("$set" << BSON(kEnabled << enable)),
        true,
        defaultMajorityWriteConcernDoNotUse());

    Status refreshStatus = refreshAndCheck(opCtx);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    if (!updateStatus.isOK() && (shouldAutoMerge() != enable)) {
        return updateStatus.getStatus().withContext(
            str::stream() << "Failed to " << (enable ? "enable" : "disable") << " auto merge");
    }

    return Status::OK();
}

bool BalancerConfiguration::shouldBalance(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    if (_balancerSettings.getMode() != BalancerModeEnum::kFull) {
        return false;
    }

    return isTimeInBalancingWindow(opCtx, boost::posix_time::second_clock::local_time());
}

bool BalancerConfiguration::shouldBalanceForAutoMerge(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    if (_balancerSettings.getMode() == BalancerModeEnum::kOff || !shouldAutoMerge()) {
        return false;
    }

    return isTimeInBalancingWindow(opCtx, boost::posix_time::second_clock::local_time());
}

MigrationSecondaryThrottleOptions BalancerConfiguration::getSecondaryThrottle() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    if (auto throttle = _balancerSettings.get_secondaryThrottle()) {
        return *throttle;
    }
    return MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault);
}

bool BalancerConfiguration::waitForDelete() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _balancerSettings.get_waitForDelete();
}

bool BalancerConfiguration::attemptToBalanceJumboChunks() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _balancerSettings.getAttemptToBalanceJumboChunks();
}

Status BalancerConfiguration::refreshAndCheck(OperationContext* opCtx) {
    try {
        Lock::ExclusiveLock settingsRefreshLock(opCtx, _settingsRefreshMutex);

        // Balancer configuration.
        Status balancerSettingsStatus = _refreshBalancerSettings(opCtx);
        if (!balancerSettingsStatus.isOK()) {
            return balancerSettingsStatus.withContext("Failed to refresh the balancer settings");
        }

        // Chunk size settings.
        Status chunkSizeStatus = _refreshChunkSizeSettings(opCtx);
        if (!chunkSizeStatus.isOK()) {
            return chunkSizeStatus.withContext("Failed to refresh the chunk sizes settings");
        }

        // Global auto merge settings.
        Status autoMergeStatus = _refreshAutoMergeSettings(opCtx);
        if (!autoMergeStatus.isOK()) {
            return autoMergeStatus.withContext("Failed to refresh the autoMerge settings");
        }
    } catch (DBException& e) {
        e.addContext("Failed to refresh the balancer configuration settings");
        return e.toStatus();
    }

    return Status::OK();
}

Status BalancerConfiguration::_refreshBalancerSettings(OperationContext* opCtx) {
    BalancerSettings settings = BalancerConfiguration::createDefaultSettings();

    auto settingsObjStatus =
        Grid::get(opCtx)->catalogClient()->getGlobalSettings(opCtx, kBalancerSettingKey);
    if (settingsObjStatus.isOK()) {
        auto settingsStatus = getSettingsFromBSON(opCtx, settingsObjStatus.getValue());
        if (!settingsStatus.isOK()) {
            return settingsStatus.getStatus();
        }

        settings = std::move(settingsStatus.getValue());
    } else if (settingsObjStatus != ErrorCodes::NoMatchingDocument) {
        return settingsObjStatus.getStatus();
    }

    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    _balancerSettings = std::move(settings);

    return Status::OK();
}

Status BalancerConfiguration::_refreshChunkSizeSettings(OperationContext* opCtx) {
    ChunkSizeSettingsType settings = ChunkSizeSettingsType::createDefault();

    auto settingsObjStatus =
        Grid::get(opCtx)->catalogClient()->getGlobalSettings(opCtx, ChunkSizeSettingsType::kKey);
    if (settingsObjStatus.isOK()) {
        auto settingsStatus = ChunkSizeSettingsType::fromBSON(settingsObjStatus.getValue());
        if (!settingsStatus.isOK()) {
            return settingsStatus.getStatus();
        }

        settings = std::move(settingsStatus.getValue());
    } else if (settingsObjStatus != ErrorCodes::NoMatchingDocument) {
        return settingsObjStatus.getStatus();
    }

    if (settings.getMaxChunkSizeBytes() != getMaxChunkSizeBytes()) {
        LOGV2(22640,
              "Changing MaxChunkSize setting",
              "newMaxChunkSizeMB"_attr = settings.getMaxChunkSizeBytes() / (1024 * 1024),
              "oldMaxChunkSizeMB"_attr = getMaxChunkSizeBytes() / (1024 * 1024));

        _maxChunkSizeBytes.store(settings.getMaxChunkSizeBytes());
    }

    return Status::OK();
}

Status BalancerConfiguration::_refreshAutoMergeSettings(OperationContext* opCtx) {
    AutoMergeSettingsType settings = AutoMergeSettingsType::createDefault();

    auto settingsObjStatus =
        Grid::get(opCtx)->catalogClient()->getGlobalSettings(opCtx, AutoMergeSettingsType::kKey);
    if (settingsObjStatus.isOK()) {
        auto settingsStatus = AutoMergeSettingsType::fromBSON(settingsObjStatus.getValue());
        if (!settingsStatus.isOK()) {
            return settingsStatus.getStatus();
        }

        settings = std::move(settingsStatus.getValue());
    } else if (settingsObjStatus != ErrorCodes::NoMatchingDocument) {
        return settingsObjStatus.getStatus();
    }

    if (settings.isEnabled() != shouldAutoMerge()) {
        LOGV2(7351300, "Changing auto merge settings", "enabled"_attr = settings.isEnabled());

        _shouldAutoMerge.store(settings.isEnabled());
    }

    return Status::OK();
}

BalancerSettings BalancerConfiguration::createDefaultSettings() {
    return BalancerSettings();
}

StatusWith<BalancerSettings> BalancerConfiguration::getSettingsFromBSON(OperationContext* opCtx,
                                                                        const BSONObj& obj) {

    BalancerSettings settings;
    try {
        IDLParserContext ctxt("BalancerSettings");
        settings = BalancerSettings::parse(obj, ctxt);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    {
        if (settings.getStopped()) {
            settings.setMode(BalancerModeEnum::kOff);
        }
    }

    // Handle activeWindowDOW and activeWindow logic
    {
        bool shouldValidateActiveWindow = true;
        const auto& activeWindowDOW = settings.getActiveWindowDOW();

        if (activeWindowDOW && !activeWindowDOW->empty()) {
            // DOW settings exist, check feature flag.
            bool balancerDOWWindowEnabled = feature_flags::gFeatureFlagBalancerWindowDOW.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

            if (balancerDOWWindowEnabled) {
                // Feature flag enabled, validate DOW settings.
                for (const auto& window : *activeWindowDOW) {
                    boost::posix_time::ptime startTime, stopTime;
                    if (!toPointInTime(std::string(window.getStart()), &startTime) ||
                        !toPointInTime(std::string(window.getStop()), &stopTime)) {
                        return Status(ErrorCodes::BadValue,
                                      "time format must be \"hh:mm\" in activeWindowDOW");
                    }

                    // Check that start and stop designate different time points.
                    if (startTime == stopTime) {
                        return Status(ErrorCodes::BadValue,
                                      "start and stop times must be different in activeWindowDOW");
                    }
                }
                shouldValidateActiveWindow = false;
            } else {
                LOGV2_WARNING(8248700, "Ignoring activeWindowDOW settings for versions under 8.3");
            }
        }

        // Validate activeWindow settings if DOW wasn't successfully processed
        // (either DOW doesn't exist or feature flag is disabled).
        if (shouldValidateActiveWindow) {
            const auto& activeWindow = settings.getActiveWindow();
            if (activeWindow) {
                // IDL already parsed the start/stop strings, now validate time format
                boost::posix_time::ptime startTime, stopTime;
                if (!toPointInTime(std::string(activeWindow->getStart()), &startTime) ||
                    !toPointInTime(std::string(activeWindow->getStop()), &stopTime)) {
                    return Status(ErrorCodes::BadValue,
                                  "activeWindow format is { start: \"hh:mm\" , stop: \"hh:mm\" }");
                }

                // Check that start and stop designate different time points.
                if (startTime == stopTime) {
                    return Status(ErrorCodes::BadValue, "start and stop times must be different");
                }
            }
        }
    }

    {
        if (!settings.get_secondaryThrottle()) {
            settings.set_secondaryThrottle(
                boost::make_optional(MigrationSecondaryThrottleOptions::create(
                    MigrationSecondaryThrottleOptions::kDefault)));
        }
    }

    return settings;
}


bool BalancerConfiguration::isTimeInBalancingWindow(OperationContext* opCtx,
                                                    const boost::posix_time::ptime& now) const {
    auto timeToString = [](const boost::posix_time::ptime& time) {
        std::ostringstream ss;
        ss << time;
        return ss.str();
    };

    bool balancerDOWWindowEnabled = feature_flags::gFeatureFlagBalancerWindowDOW.isEnabled(
        VersionContext::getDecoration(opCtx),
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    const auto& activeWindowDOW = _balancerSettings.getActiveWindowDOW();
    if (balancerDOWWindowEnabled && activeWindowDOW && !activeWindowDOW->empty()) {
        std::string currentDayOfWeek = getDayOfWeekString(now.date());

        // Check if the current time is in any of the day-specific windows.
        for (const auto& window : *activeWindowDOW) {
            std::string windowDayOfWeek = std::string(DayOfWeek_serializer(window.getDay()));
            if (windowDayOfWeek == currentDayOfWeek) {
                boost::posix_time::ptime startTime, stopTime;
                toPointInTime(std::string(window.getStart()), &startTime);
                toPointInTime(std::string(window.getStop()), &stopTime);
                boost::posix_time::ptime windowStartForToday(now.date(), startTime.time_of_day());
                boost::posix_time::ptime windowStopForToday(now.date(), stopTime.time_of_day());

                LOGV2_DEBUG(10806100,
                            1,
                            "inBalancingWindow",
                            "now"_attr = timeToString(now),
                            "activeWindowStart"_attr = timeToString(windowStartForToday),
                            "activeWindowStop"_attr = timeToString(windowStopForToday),
                            "dayOfWeek"_attr = currentDayOfWeek);

                if (windowStopForToday > windowStartForToday) {
                    if (now >= windowStartForToday && now <= windowStopForToday) {
                        return true;
                    }
                } else {
                    if (now >= windowStartForToday || now <= windowStopForToday) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    const auto& activeWindow = _balancerSettings.getActiveWindow();
    if (!activeWindow) {
        return true;
    }

    boost::posix_time::ptime activeWindowStart, activeWindowStop;
    if (!toPointInTime(std::string(activeWindow->getStart()), &activeWindowStart) ||
        !toPointInTime(std::string(activeWindow->getStop()), &activeWindowStop)) {
        return true;
    }

    LOGV2_DEBUG(24094,
                1,
                "inBalancingWindow",
                "now"_attr = timeToString(now),
                "activeWindowStart"_attr = timeToString(activeWindowStart),
                "activeWindowStop"_attr = timeToString(activeWindowStop));

    if (activeWindowStop > activeWindowStart) {
        if ((now >= activeWindowStart) && (now <= activeWindowStop)) {
            return true;
        }
    } else if (activeWindowStart > activeWindowStop) {
        if ((now >= activeWindowStart) || (now <= activeWindowStop)) {
            return true;
        }
    } else {
        MONGO_UNREACHABLE_TASSERT(10083533);
    }

    return false;
}

ChunkSizeSettingsType::ChunkSizeSettingsType() = default;

ChunkSizeSettingsType ChunkSizeSettingsType::createDefault() {
    return ChunkSizeSettingsType();
}

StatusWith<ChunkSizeSettingsType> ChunkSizeSettingsType::fromBSON(const BSONObj& obj) {
    long long maxChunkSizeMB;
    Status status = bsonExtractIntegerField(obj, kValue, &maxChunkSizeMB);
    if (!status.isOK())
        return status;

    const uint64_t maxChunkSizeBytes = maxChunkSizeMB * 1024 * 1024;

    if (!checkMaxChunkSizeValid(maxChunkSizeBytes)) {
        return {ErrorCodes::BadValue,
                str::stream() << maxChunkSizeMB << " is not a valid value for " << kKey};
    }

    ChunkSizeSettingsType settings;
    settings._maxChunkSizeBytes = maxChunkSizeBytes;

    return settings;
}

bool ChunkSizeSettingsType::checkMaxChunkSizeValid(uint64_t maxChunkSizeBytes) {
    if (maxChunkSizeBytes >= (1024 * 1024) && maxChunkSizeBytes <= (1024 * 1024 * 1024)) {
        return true;
    }

    return false;
}

StatusWith<AutoMergeSettingsType> AutoMergeSettingsType::fromBSON(const BSONObj& obj) {
    bool isEnabled;
    Status status = bsonExtractBooleanField(obj, kEnabled, &isEnabled);
    if (!status.isOK())
        return status;

    AutoMergeSettingsType settings;
    settings._isEnabled = isEnabled;

    return settings;
}

}  // namespace mongo
