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
#include <algorithm>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/date_time/time_duration.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/operators.hpp>
#include <cstdio>
#include <deque>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

// parses time of day in "hh:mm" format assuming 'hh' is 00-23
bool toPointInTime(const std::string& str, boost::posix_time::ptime* timeOfDay) {
    int hh = 0;
    int mm = 0;
    if (2 != sscanf(str.c_str(), "%d:%d", &hh, &mm)) {
        return false;
    }

    // verify that time is well formed
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

}  // namespace

const char BalancerSettingsType::kKey[] = "balancer";
const char* BalancerSettingsType::kBalancerModes[] = {"full", "off"};

const char ChunkSizeSettingsType::kKey[] = "chunksize";
const uint64_t ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes{128 * 1024 * 1024};

const char AutoMergeSettingsType::kKey[] = "automerge";

BalancerConfiguration::BalancerConfiguration()
    : _balancerSettings(BalancerSettingsType::createDefault()),
      _maxChunkSizeBytes(ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes),
      _shouldAutoMerge(true) {}

BalancerConfiguration::~BalancerConfiguration() = default;

BalancerSettingsType::BalancerMode BalancerConfiguration::getBalancerMode() const {
    stdx::lock_guard<Latch> lk(_balancerSettingsMutex);
    return _balancerSettings.getMode();
}

Status BalancerConfiguration::setBalancerMode(OperationContext* opCtx,
                                              BalancerSettingsType::BalancerMode mode) {
    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        NamespaceString::kConfigSettingsNamespace,
        BSON("_id" << BalancerSettingsType::kKey),
        BSON("$set" << BSON(kStopped << (mode == BalancerSettingsType::kOff) << kMode
                                     << BalancerSettingsType::kBalancerModes[mode])),
        true,
        ShardingCatalogClient::kMajorityWriteConcern);

    Status refreshStatus = refreshAndCheck(opCtx);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    if (!updateStatus.isOK() && (getBalancerMode() != mode)) {
        return updateStatus.getStatus().withContext(str::stream()
                                                    << "Failed to set the balancer mode to "
                                                    << BalancerSettingsType::kBalancerModes[mode]);
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
        ShardingCatalogClient::kMajorityWriteConcern);

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

bool BalancerConfiguration::shouldBalance() const {
    stdx::lock_guard<Latch> lk(_balancerSettingsMutex);
    if (_balancerSettings.getMode() != BalancerSettingsType::kFull) {
        return false;
    }

    return _balancerSettings.isTimeInBalancingWindow(boost::posix_time::second_clock::local_time());
}

bool BalancerConfiguration::shouldBalanceForAutoMerge() const {
    stdx::lock_guard<Latch> lk(_balancerSettingsMutex);
    if (_balancerSettings.getMode() == BalancerSettingsType::kOff || !shouldAutoMerge()) {
        return false;
    }

    return _balancerSettings.isTimeInBalancingWindow(boost::posix_time::second_clock::local_time());
}

MigrationSecondaryThrottleOptions BalancerConfiguration::getSecondaryThrottle() const {
    stdx::lock_guard<Latch> lk(_balancerSettingsMutex);
    return _balancerSettings.getSecondaryThrottle();
}

bool BalancerConfiguration::waitForDelete() const {
    stdx::lock_guard<Latch> lk(_balancerSettingsMutex);
    return _balancerSettings.waitForDelete();
}

bool BalancerConfiguration::attemptToBalanceJumboChunks() const {
    stdx::lock_guard<Latch> lk(_balancerSettingsMutex);
    return _balancerSettings.attemptToBalanceJumboChunks();
}

Status BalancerConfiguration::refreshAndCheck(OperationContext* opCtx) {
    // Balancer configuration
    Status balancerSettingsStatus = _refreshBalancerSettings(opCtx);
    if (!balancerSettingsStatus.isOK()) {
        return balancerSettingsStatus.withContext("Failed to refresh the balancer settings");
    }

    // Chunk size settings
    Status chunkSizeStatus = _refreshChunkSizeSettings(opCtx);
    if (!chunkSizeStatus.isOK()) {
        return chunkSizeStatus.withContext("Failed to refresh the chunk sizes settings");
    }

    // Global auto merge settings
    Status autoMergeStatus = _refreshAutoMergeSettings(opCtx);
    if (!autoMergeStatus.isOK()) {
        return autoMergeStatus.withContext("Failed to refresh the autoMerge settings");
    }

    return Status::OK();
}

Status BalancerConfiguration::_refreshBalancerSettings(OperationContext* opCtx) {
    BalancerSettingsType settings = BalancerSettingsType::createDefault();

    auto settingsObjStatus =
        Grid::get(opCtx)->catalogClient()->getGlobalSettings(opCtx, BalancerSettingsType::kKey);
    if (settingsObjStatus.isOK()) {
        auto settingsStatus = BalancerSettingsType::fromBSON(settingsObjStatus.getValue());
        if (!settingsStatus.isOK()) {
            return settingsStatus.getStatus();
        }

        settings = std::move(settingsStatus.getValue());
    } else if (settingsObjStatus != ErrorCodes::NoMatchingDocument) {
        return settingsObjStatus.getStatus();
    }

    stdx::lock_guard<Latch> lk(_balancerSettingsMutex);
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
              "Changing MaxChunkSize setting to {newMaxChunkSizeMB}MB from {oldMaxChunkSizeMB}MB",
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

BalancerSettingsType::BalancerSettingsType()
    : _secondaryThrottle(
          MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault)) {}

BalancerSettingsType BalancerSettingsType::createDefault() {
    return BalancerSettingsType();
}

StatusWith<BalancerSettingsType> BalancerSettingsType::fromBSON(const BSONObj& obj) {
    BalancerSettingsType settings;

    {
        bool stopped;
        Status status = bsonExtractBooleanFieldWithDefault(obj, kStopped, false, &stopped);
        if (!status.isOK())
            return status;
        if (stopped) {
            settings._mode = kOff;
        } else {
            std::string modeStr;
            status = bsonExtractStringFieldWithDefault(obj, kMode, kBalancerModes[kFull], &modeStr);
            if (!status.isOK())
                return status;
            auto it = std::find(std::begin(kBalancerModes), std::end(kBalancerModes), modeStr);
            if (it == std::end(kBalancerModes)) {
                std::vector<std::string> supportedModes;
                std::transform(std::begin(kBalancerModes),
                               std::end(kBalancerModes),
                               std::back_inserter(supportedModes),
                               [](const char* supportedMode) -> std::string {
                                   return std::string(supportedMode);
                               });
                LOGV2_WARNING(
                    7575700,
                    "Balancer turned off because currently set balancing mode is not valid",
                    "currentMode"_attr = modeStr,
                    "supportedModes"_attr = supportedModes);
                settings._mode = kOff;
            } else {
                settings._mode = static_cast<BalancerMode>(it - std::begin(kBalancerModes));
            }
        }
    }

    {
        BSONElement activeWindowElem;
        Status status = bsonExtractTypedField(obj, kActiveWindow, Object, &activeWindowElem);
        if (status.isOK()) {
            const BSONObj balancingWindowObj = activeWindowElem.Obj();
            if (balancingWindowObj.isEmpty()) {
                return Status(ErrorCodes::BadValue, "activeWindow not specified");
            }

            // Check if both 'start' and 'stop' are present
            const std::string start = balancingWindowObj.getField("start").str();
            const std::string stop = balancingWindowObj.getField("stop").str();

            if (start.empty() || stop.empty()) {
                return Status(ErrorCodes::BadValue,
                              str::stream()
                                  << "must specify both start and stop of balancing window: "
                                  << balancingWindowObj);
            }

            // Check that both 'start' and 'stop' are valid time-of-day
            boost::posix_time::ptime startTime;
            boost::posix_time::ptime stopTime;
            if (!toPointInTime(start, &startTime) || !toPointInTime(stop, &stopTime)) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << kActiveWindow << " format is "
                                            << " { start: \"hh:mm\" , stop: \"hh:mm\" }");
            }

            // Check that start and stop designate different time points
            if (startTime == stopTime) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "start and stop times must be different");
            }

            settings._activeWindowStart = startTime;
            settings._activeWindowStop = stopTime;
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }

    {
        auto secondaryThrottleStatus =
            MigrationSecondaryThrottleOptions::createFromBalancerConfig(obj);
        if (!secondaryThrottleStatus.isOK()) {
            return secondaryThrottleStatus.getStatus();
        }

        settings._secondaryThrottle = std::move(secondaryThrottleStatus.getValue());
    }

    {
        bool waitForDelete;
        Status status =
            bsonExtractBooleanFieldWithDefault(obj, kWaitForDelete, false, &waitForDelete);
        if (!status.isOK())
            return status;

        settings._waitForDelete = waitForDelete;
    }

    {
        bool attemptToBalanceJumboChunks;
        Status status = bsonExtractBooleanFieldWithDefault(
            obj, kAttemptToBalanceJumboChunks, false, &attemptToBalanceJumboChunks);
        if (!status.isOK())
            return status;

        settings._attemptToBalanceJumboChunks = attemptToBalanceJumboChunks;
    }

    return settings;
}

bool BalancerSettingsType::isTimeInBalancingWindow(const boost::posix_time::ptime& now) const {
    invariant(!_activeWindowStart == !_activeWindowStop);

    if (!_activeWindowStart) {
        return true;
    }

    auto timeToString = [](const boost::posix_time::ptime& time) {
        std::ostringstream ss;
        ss << time;
        return ss.str();
    };
    LOGV2_DEBUG(24094,
                1,
                "inBalancingWindow",
                "now"_attr = timeToString(now),
                "activeWindowStart"_attr = timeToString(*_activeWindowStart),
                "activeWindowStop"_attr = timeToString(*_activeWindowStop));

    if (*_activeWindowStop > *_activeWindowStart) {
        if ((now >= *_activeWindowStart) && (now <= *_activeWindowStop)) {
            return true;
        }
    } else if (*_activeWindowStart > *_activeWindowStop) {
        if ((now >= *_activeWindowStart) || (now <= *_activeWindowStop)) {
            return true;
        }
    } else {
        MONGO_UNREACHABLE;
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
