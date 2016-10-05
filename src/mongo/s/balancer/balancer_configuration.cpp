/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/balancer/balancer_configuration.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kValue[] = "value";
const char kEnabled[] = "enabled";
const char kStopped[] = "stopped";
const char kMode[] = "mode";
const char kActiveWindow[] = "activeWindow";
const char kWaitForDelete[] = "_waitForDelete";

const NamespaceString kSettingsNamespace("config", "settings");

}  // namespace

const char BalancerSettingsType::kKey[] = "balancer";
const char* BalancerSettingsType::kBalancerModes[] = {"full", "autoSplitOnly", "off"};

const char ChunkSizeSettingsType::kKey[] = "chunksize";
const uint64_t ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes{64 * 1024 * 1024};

const char AutoSplitSettingsType::kKey[] = "autosplit";

BalancerConfiguration::BalancerConfiguration()
    : _balancerSettings(BalancerSettingsType::createDefault()),
      _maxChunkSizeBytes(ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes),
      _shouldAutoSplit(true) {}

BalancerConfiguration::~BalancerConfiguration() = default;

BalancerSettingsType::BalancerMode BalancerConfiguration::getBalancerMode() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _balancerSettings.getMode();
}

Status BalancerConfiguration::setBalancerMode(OperationContext* txn,
                                              BalancerSettingsType::BalancerMode mode) {
    auto updateStatus = Grid::get(txn)->catalogClient(txn)->updateConfigDocument(
        txn,
        kSettingsNamespace.ns(),
        BSON("_id" << BalancerSettingsType::kKey),
        BSON("$set" << BSON(kStopped << (mode == BalancerSettingsType::kOff) << kMode
                                     << BalancerSettingsType::kBalancerModes[mode])),
        true,
        ShardingCatalogClient::kMajorityWriteConcern);

    Status refreshStatus = refreshAndCheck(txn);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    if (!updateStatus.isOK() && (getBalancerMode() != mode)) {
        return {updateStatus.getStatus().code(),
                str::stream() << "Failed to update balancer configuration due to "
                              << updateStatus.getStatus().reason()};
    }

    return Status::OK();
}

bool BalancerConfiguration::shouldBalance() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    if (_balancerSettings.getMode() == BalancerSettingsType::kOff ||
        _balancerSettings.getMode() == BalancerSettingsType::kAutoSplitOnly) {
        return false;
    }

    return _balancerSettings.isTimeInBalancingWindow(boost::posix_time::second_clock::local_time());
}

bool BalancerConfiguration::shouldBalanceForAutoSplit() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    if (_balancerSettings.getMode() == BalancerSettingsType::kOff) {
        return false;
    }

    return _balancerSettings.isTimeInBalancingWindow(boost::posix_time::second_clock::local_time());
}

MigrationSecondaryThrottleOptions BalancerConfiguration::getSecondaryThrottle() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _balancerSettings.getSecondaryThrottle();
}

bool BalancerConfiguration::waitForDelete() const {
    stdx::lock_guard<stdx::mutex> lk(_balancerSettingsMutex);
    return _balancerSettings.waitForDelete();
}

Status BalancerConfiguration::refreshAndCheck(OperationContext* txn) {
    // Balancer configuration
    Status balancerSettingsStatus = _refreshBalancerSettings(txn);
    if (!balancerSettingsStatus.isOK()) {
        return {balancerSettingsStatus.code(),
                str::stream() << "Failed to refresh the balancer settings due to "
                              << balancerSettingsStatus.toString()};
    }

    // Chunk size settings
    Status chunkSizeStatus = _refreshChunkSizeSettings(txn);
    if (!chunkSizeStatus.isOK()) {
        return {chunkSizeStatus.code(),
                str::stream() << "Failed to refresh the chunk sizes settings due to "
                              << chunkSizeStatus.toString()};
    }

    // AutoSplit settings
    Status autoSplitStatus = _refreshAutoSplitSettings(txn);
    if (!autoSplitStatus.isOK()) {
        return {autoSplitStatus.code(),
                str::stream() << "Failed to refresh the autoSplit settings due to "
                              << autoSplitStatus.toString()};
    }

    return Status::OK();
}

Status BalancerConfiguration::_refreshBalancerSettings(OperationContext* txn) {
    BalancerSettingsType settings = BalancerSettingsType::createDefault();

    auto settingsObjStatus =
        Grid::get(txn)->catalogClient(txn)->getGlobalSettings(txn, BalancerSettingsType::kKey);
    if (settingsObjStatus.isOK()) {
        auto settingsStatus = BalancerSettingsType::fromBSON(settingsObjStatus.getValue());
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

Status BalancerConfiguration::_refreshChunkSizeSettings(OperationContext* txn) {
    ChunkSizeSettingsType settings = ChunkSizeSettingsType::createDefault();

    auto settingsObjStatus =
        grid.catalogClient(txn)->getGlobalSettings(txn, ChunkSizeSettingsType::kKey);
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
        log() << "MaxChunkSize changing from " << getMaxChunkSizeBytes() / (1024 * 1024) << "MB"
              << " to " << settings.getMaxChunkSizeBytes() / (1024 * 1024) << "MB";

        _maxChunkSizeBytes.store(settings.getMaxChunkSizeBytes());
    }

    return Status::OK();
}

Status BalancerConfiguration::_refreshAutoSplitSettings(OperationContext* txn) {
    AutoSplitSettingsType settings = AutoSplitSettingsType::createDefault();

    auto settingsObjStatus =
        grid.catalogClient(txn)->getGlobalSettings(txn, AutoSplitSettingsType::kKey);
    if (settingsObjStatus.isOK()) {
        auto settingsStatus = AutoSplitSettingsType::fromBSON(settingsObjStatus.getValue());
        if (!settingsStatus.isOK()) {
            return settingsStatus.getStatus();
        }

        settings = std::move(settingsStatus.getValue());
    } else if (settingsObjStatus != ErrorCodes::NoMatchingDocument) {
        return settingsObjStatus.getStatus();
    }

    if (settings.getShouldAutoSplit() != getShouldAutoSplit()) {
        log() << "ShouldAutoSplit changing from " << getShouldAutoSplit() << " to "
              << settings.getShouldAutoSplit();

        _shouldAutoSplit.store(settings.getShouldAutoSplit());
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
                return Status(ErrorCodes::BadValue, "Invalid balancer mode");
            }

            settings._mode = static_cast<BalancerMode>(it - std::begin(kBalancerModes));
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

    return settings;
}

bool BalancerSettingsType::isTimeInBalancingWindow(const boost::posix_time::ptime& now) const {
    invariant(!_activeWindowStart == !_activeWindowStop);

    if (!_activeWindowStart) {
        return true;
    }

    LOG(1).stream() << "inBalancingWindow: "
                    << " now: " << now << " startTime: " << *_activeWindowStart
                    << " stopTime: " << *_activeWindowStop;

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

AutoSplitSettingsType::AutoSplitSettingsType() = default;

AutoSplitSettingsType AutoSplitSettingsType::createDefault() {
    return AutoSplitSettingsType();
}

StatusWith<AutoSplitSettingsType> AutoSplitSettingsType::fromBSON(const BSONObj& obj) {
    bool shouldAutoSplit;
    Status status = bsonExtractBooleanField(obj, kEnabled, &shouldAutoSplit);
    if (!status.isOK())
        return status;

    AutoSplitSettingsType settings;
    settings._shouldAutoSplit = shouldAutoSplit;

    return settings;
}

}  // namespace mongo
