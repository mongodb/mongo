/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_settings.h"

#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

    const std::string SettingsType::ConfigNS = "config.settings";
    const std::string SettingsType::BalancerDocKey("balancer");
    const std::string SettingsType::ChunkSizeDocKey("chunksize");

    const BSONField<std::string> SettingsType::key("_id");
    const BSONField<long long> SettingsType::chunkSize("value");
    const BSONField<bool> SettingsType::balancerStopped("stopped");
    const BSONField<BSONObj> SettingsType::balancerActiveWindow("activeWindow");
    const BSONField<bool> SettingsType::deprecated_secondaryThrottle("_secondaryThrottle");
    const BSONField<BSONObj> SettingsType::migrationWriteConcern("_secondaryThrottle");
    const BSONField<bool> SettingsType::waitForDelete("_waitForDelete");

    StatusWith<SettingsType> SettingsType::fromBSON(const BSONObj& source) {
        SettingsType settings;

        {
            std::string settingsKey;
            Status status = bsonExtractStringField(source, key.name(), &settingsKey);
            if (!status.isOK()) return status;
            settings._key = settingsKey;
        }

        if (settings._key == ChunkSizeDocKey) {
            long long settingsChunkSize;
            Status status = bsonExtractIntegerField(source,
                                                    chunkSize.name(),
                                                    &settingsChunkSize);
            if (!status.isOK()) return status;
            settings._chunkSize = settingsChunkSize;
        }
        else if (settings._key == BalancerDocKey) {
            {
                bool settingsBalancerStopped;
                Status status = bsonExtractBooleanFieldWithDefault(source,
                                                                   balancerStopped.name(),
                                                                   false,
                                                                   &settingsBalancerStopped);
                if (!status.isOK()) return status;
                settings._balancerStopped = settingsBalancerStopped;
            }

            {
                BSONElement settingsBalancerActiveWindowElem;
                Status status = bsonExtractTypedField(source,
                                                      balancerActiveWindow.name(),
                                                      Object,
                                                      &settingsBalancerActiveWindowElem);
                if (status != ErrorCodes::NoSuchKey) {
                    if (!status.isOK()) return status;
                    StatusWith<BoostTimePair> timePairResult =
                        settings._parseBalancingWindow(settingsBalancerActiveWindowElem.Obj());
                    if (!timePairResult.isOK()) return timePairResult.getStatus();
                    settings._balancerActiveWindow = timePairResult.getValue();
                }
            }

            {
                BSONElement settingsMigrationWriteConcernElem;
                Status status = bsonExtractTypedField(source,
                                                      migrationWriteConcern.name(),
                                                      Object,
                                                      &settingsMigrationWriteConcernElem);
                if (status == ErrorCodes::TypeMismatch) {
                    bool settingsSecondaryThrottle;
                    status = bsonExtractBooleanFieldWithDefault(source,
                                                                deprecated_secondaryThrottle
                                                                    .name(),
                                                                true,
                                                                &settingsSecondaryThrottle);
                    if (!status.isOK()) return status;
                    settings._secondaryThrottle = settingsSecondaryThrottle;
                }
                else if (status != ErrorCodes::NoSuchKey) {
                    if (!status.isOK()) return status;
                    settings._migrationWriteConcern = WriteConcernOptions();
                    status = settings._migrationWriteConcern->parse(
                        settingsMigrationWriteConcernElem.Obj()
                    );
                    if (!status.isOK()) return status;
                }
            }

            {
                bool settingsWaitForDelete;
                Status status = bsonExtractBooleanField(source,
                                                        waitForDelete.name(),
                                                        &settingsWaitForDelete);
                if (status != ErrorCodes::NoSuchKey) {
                    if (!status.isOK()) return status;
                    settings._waitForDelete = settingsWaitForDelete;
                }
            }
        }

        return settings;
    }

    Status SettingsType::validate() const {
        if (!_key.is_initialized() || _key->empty()) {
            return Status(ErrorCodes::NoSuchKey,
                          str::stream() << "missing " << key.name() << " field");
        }

        if (_key == ChunkSizeDocKey) {
            if (!(getChunkSize() > 0)) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "chunksize specified in " << chunkSize.name()
                                            << " field must be greater than zero");
            }
        }
        else if (_key == BalancerDocKey) {
            if (_secondaryThrottle.is_initialized() &&
                _migrationWriteConcern.is_initialized()) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "cannot have both secondary throttle and "
                                            << "migration write concern set at the same time");
            }
        }
        else {
            return Status(ErrorCodes::UnsupportedFormat,
                          str::stream() << "unsupported key in " << key.name() << " field");
        }

        return Status::OK();
    }

    BSONObj SettingsType::toBSON() const {
        BSONObjBuilder builder;

        if (_key) builder.append(key(), getKey());
        if (_chunkSize) builder.append(chunkSize(), getChunkSize());
        if (_balancerStopped) builder.append(balancerStopped(), getBalancerStopped());
        if (_secondaryThrottle) {
            builder.append(deprecated_secondaryThrottle(), getSecondaryThrottle());
        }
        if (_migrationWriteConcern) {
            builder.append(migrationWriteConcern(), getMigrationWriteConcern().toBSON());
        }
        if (_waitForDelete) builder.append(waitForDelete(), getWaitForDelete());

        return builder.obj();
    }

    std::string SettingsType::toString() const {
        return toBSON().toString();
    }

    std::unique_ptr<WriteConcernOptions> SettingsType::getWriteConcern() const {
        dassert(_key.is_initialized());
        dassert(_key == BalancerDocKey);

        if (isSecondaryThrottleSet() && !getSecondaryThrottle()) {
            return stdx::make_unique<WriteConcernOptions>(1, WriteConcernOptions::NONE, 0);
        }
        else if (!isMigrationWriteConcernSet()) {
            // Default setting.
            return nullptr;
        }
        else {
            return stdx::make_unique<WriteConcernOptions>(getMigrationWriteConcern());
        }
    }

    StatusWith<BoostTimePair> SettingsType::_parseBalancingWindow(const BSONObj& balancingWindowObj) {
        if (balancingWindowObj.isEmpty()) {
            return Status(ErrorCodes::BadValue,
                          "'activeWindow' can't be empty");
        }

        // check if both 'start' and 'stop' are present
        std::string start = balancingWindowObj.getField("start").str();
        std::string stop = balancingWindowObj.getField("stop").str();
        if (start.empty() || stop.empty()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "must specify both start and end of balancing window: "
                                        << balancingWindowObj);
        }

        // check that both 'start' and 'stop' are valid time-of-day
        boost::posix_time::ptime startTime, stopTime;
        if (!toPointInTime(start, &startTime) || !toPointInTime(stop, &stopTime)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << balancerActiveWindow.name() << " format is "
                                        << " { start: \"hh:mm\" , stop: \"hh:mm\" }");
        }

        return std::make_pair(startTime, stopTime);
    }

    bool SettingsType::inBalancingWindow(const boost::posix_time::ptime& now) const {
        if (!_balancerActiveWindow.is_initialized()) {
            return true;
        }
        const boost::posix_time::ptime& startTime = _balancerActiveWindow->first;
        const boost::posix_time::ptime& stopTime = _balancerActiveWindow->second;

        LOG(1).stream() << "inBalancingWindow: "
                        << " now: " << now
                        << " startTime: " << startTime
                        << " stopTime: " << stopTime;

        // allow balancing if during the activeWindow
        // note that a window may be open during the night
        if (stopTime > startTime) {
            if ((now >= startTime) && (now <= stopTime)) {
                return true;
            }
        }
        else if (startTime > stopTime) {
            if ((now >= startTime) || (now <= stopTime)) {
                return true;
            }
        }

        return false;
    }

    void SettingsType::setKey(const std::string& key) {
        invariant(!key.empty());
        _key = key;
    }

    void SettingsType::setChunkSize(const long long chunkSize) {
        invariant(_key == ChunkSizeDocKey);
        invariant(chunkSize > 0);
        _chunkSize = chunkSize;
    }

    void SettingsType::setBalancerStopped(const bool balancerStopped) {
        invariant(_key == BalancerDocKey);
        _balancerStopped = balancerStopped;
    }

    void SettingsType::setBalancerActiveWindow(const BSONObj& balancerActiveWindow) {
        invariant(_key == BalancerDocKey);
        StatusWith<BoostTimePair> timePairResult = _parseBalancingWindow(balancerActiveWindow);
        invariant(timePairResult.isOK());
        _balancerActiveWindow = timePairResult.getValue();
    }

    void SettingsType::setSecondaryThrottle(const bool secondaryThrottle) {
        invariant(_key == BalancerDocKey);
        _secondaryThrottle = secondaryThrottle;
    }

    void SettingsType::setMigrationWriteConcern(const BSONObj& migrationWCBSONObj) {
        invariant(_key == BalancerDocKey);
        invariant(!migrationWCBSONObj.isEmpty());
        Status status = _migrationWriteConcern->parse(migrationWCBSONObj);
        invariant(status.isOK());
    }

    void SettingsType::setWaitForDelete(const bool waitForDelete) {
        invariant(_key == BalancerDocKey);
        _waitForDelete = waitForDelete;
    }

} // namespace mongo
