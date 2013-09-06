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
#include "mongo/s/type_settings.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string SettingsType::ConfigNS = "config.settings";

    const BSONField<std::string> SettingsType::key("_id");
    const BSONField<int> SettingsType::chunksize("value");
    const BSONField<bool> SettingsType::balancerStopped("stopped");
    const BSONField<BSONObj> SettingsType::balancerActiveWindow("activeWindow");
    const BSONField<bool> SettingsType::shortBalancerSleep("_nosleep");
    const BSONField<bool> SettingsType::secondaryThrottle("_secondaryThrottle");

    SettingsType::SettingsType() {
        clear();
    }

    SettingsType::~SettingsType() {
    }

    bool SettingsType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isKeySet) {
            *errMsg = stream() << "missing " << key.name() << " field";
            return false;
        }

        if (_key == "chunksize") {
            if (_isChunksizeSet) {
                if (!(_chunksize > 0)) {
                    *errMsg = stream() << "chunksize specified in " << chunksize.name() <<
                                          " field must be greater than zero";
                    return false;
                }
            } else {
                *errMsg = stream() << "chunksize must be specified in " << chunksize.name() <<
                                      " field for chunksize setting";
                return false;
            }
            return true;
        }
        else if (_key == "balancer") {
            if (_balancerActiveWindow.nFields() != 0) {
                // check if both 'start' and 'stop' are present
                const std::string start = _balancerActiveWindow["start"].str();
                const std::string stop = _balancerActiveWindow["stop"].str();
                if ( start.empty() || stop.empty() ) {
                    *errMsg = stream() << balancerActiveWindow.name() <<
                                          " format is { start: \"hh:mm\" , stop: \"hh:mm\" }";
                    return false;
                }

                // check that both 'start' and 'stop' are valid time-of-day
                boost::posix_time::ptime startTime, stopTime;
                if ( !toPointInTime( start , &startTime ) || !toPointInTime( stop , &stopTime ) ) {
                    *errMsg = stream() << balancerActiveWindow.name() <<
                                          " format is { start: \"hh:mm\" , stop: \"hh:mm\" }";
                    return false;
                }
            }
            return true;
        }
        else {
            *errMsg = stream() << "unsupported key in  " << key.name() << " field";
            return false;
        }
    }

    BSONObj SettingsType::toBSON() const {
        BSONObjBuilder builder;

        if (_isKeySet) builder.append(key(), _key);
        if (_isChunksizeSet) builder.append(chunksize(), _chunksize);
        if (_isBalancerStoppedSet) builder.append(balancerStopped(), _balancerStopped);
        if (_isBalancerActiveWindowSet) {
            builder.append(balancerActiveWindow(), _balancerActiveWindow);
        }
        if (_isShortBalancerSleepSet) builder.append(shortBalancerSleep(), _shortBalancerSleep);
        if (_isSecondaryThrottleSet) builder.append(secondaryThrottle(), _secondaryThrottle);

        return builder.obj();
    }

    bool SettingsType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, key, &_key, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isKeySet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, chunksize, &_chunksize, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isChunksizeSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, balancerStopped, &_balancerStopped, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isBalancerStoppedSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, balancerActiveWindow,
                                          &_balancerActiveWindow, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isBalancerActiveWindowSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, shortBalancerSleep, &_shortBalancerSleep, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isShortBalancerSleepSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, secondaryThrottle, &_secondaryThrottle, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isSecondaryThrottleSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void SettingsType::clear() {

        _key.clear();
        _isKeySet = false;

        _chunksize = 0;
        _isChunksizeSet = false;

        _balancerStopped = false;
        _isBalancerStoppedSet = false;

        _balancerActiveWindow = BSONObj();
        _isBalancerActiveWindowSet = false;

        _shortBalancerSleep = false;
        _isShortBalancerSleepSet = false;

        _secondaryThrottle = false;
        _isSecondaryThrottleSet = false;

    }

    void SettingsType::cloneTo(SettingsType* other) const {
        other->clear();

        other->_key = _key;
        other->_isKeySet = _isKeySet;

        other->_chunksize = _chunksize;
        other->_isChunksizeSet = _isChunksizeSet;

        other->_balancerStopped = _balancerStopped;
        other->_isBalancerStoppedSet = _isBalancerStoppedSet;

        other->_balancerActiveWindow = _balancerActiveWindow;
        other->_isBalancerActiveWindowSet = _isBalancerActiveWindowSet;

        other->_shortBalancerSleep = _shortBalancerSleep;
        other->_isShortBalancerSleepSet = _isShortBalancerSleepSet;

        other->_secondaryThrottle = _secondaryThrottle;
        other->_isSecondaryThrottleSet = _isSecondaryThrottleSet;

    }

    std::string SettingsType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
