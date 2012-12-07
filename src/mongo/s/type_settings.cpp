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
 */

#include "mongo/s/type_settings.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string SettingsType::ConfigNS = "config.settings";

    BSONField<std::string> SettingsType::key("_id");
    BSONField<int> SettingsType::chunksize("value");
    BSONField<bool> SettingsType::balancerStopped("stopped");
    BSONField<BSONObj> SettingsType::balancerActiveWindow("activeWindow");
    BSONField<bool> SettingsType::shortBalancerSleep("_nosleep");
    BSONField<bool> SettingsType::secondaryThrottle("_secondaryThrottle");

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

        if (_key.empty()) {
            *errMsg = stream() << "missing " << key.name() << " field";
            return false;
        }

        if (_key == "chunksize") {
            if (!(_chunksize > 0)) {
                *errMsg = stream() << "chunksize specified in " << chunksize.name() <<
                                      " field must be greater than zero";
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
                if ( ! toPointInTime( start , &startTime ) || ! toPointInTime( stop , &stopTime ) ) {
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

        if (!_key.empty()) builder.append(key(), _key);
        if (_chunksize > 0) builder.append(chunksize(), _chunksize);
        builder.append(balancerStopped(), _balancerStopped);
        if (_balancerActiveWindow.nFields()) builder.append(balancerActiveWindow(),
                                                            _balancerActiveWindow);
        builder.append(shortBalancerSleep(), _shortBalancerSleep);
        builder.append(secondaryThrottle(), _secondaryThrottle);

        return builder.obj();
    }

    void SettingsType::parseBSON(BSONObj source) {
        clear();

        bool ok = true;
        ok &= FieldParser::extract(source, key, "", &_key);
        ok &= FieldParser::extract(source, chunksize, 0, &_chunksize);
        ok &= FieldParser::extract(source, balancerStopped, false, &_balancerStopped);
        ok &= FieldParser::extract(source, balancerActiveWindow, BSONObj(), &_balancerActiveWindow);
        ok &= FieldParser::extract(source, shortBalancerSleep, false, &_shortBalancerSleep);
        ok &= FieldParser::extract(source, secondaryThrottle, false, &_secondaryThrottle);
        if (! ok) {
            clear();
        }
    }

    void SettingsType::clear() {
        _key.clear();
        _chunksize = 0;
        _balancerStopped = false;
        _balancerActiveWindow = BSONObj();
        _shortBalancerSleep = false;
        _secondaryThrottle = false;
    }

    void SettingsType::cloneTo(SettingsType* other) {
        other->clear();
        other->_key = _key;
        other->_chunksize = _chunksize;
        other->_balancerStopped = _balancerStopped;
        other->_balancerActiveWindow = _balancerActiveWindow;
        other->_shortBalancerSleep = _shortBalancerSleep;
        other->_secondaryThrottle = _secondaryThrottle;
    }

} // namespace mongo
