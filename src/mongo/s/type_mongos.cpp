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

#include "mongo/s/type_mongos.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string MongosType::ConfigNS = "config.mongos";

    BSONField<string> MongosType::name("_id");
    BSONField<Date_t> MongosType::ping("ping");
    BSONField<int> MongosType::up("up");
    BSONField<bool> MongosType::waiting("waiting");

    MongosType::MongosType() {
        clear();
    }

    MongosType::~MongosType() {
    }

    bool MongosType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        if (_name.empty()) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        else {
            // TODO: check that string has valid "host:port" format
        }
        if (_ping.millis == 0) {
            *errMsg = stream() << "missing " << ping.name() << " field";
            return false;
        }
        if (!(_up > 0)) {
            *errMsg = stream() << "uptime must be positive";
            return false;
        }

        return true;
    }

    BSONObj MongosType::toBSON() const {
        BSONObjBuilder builder;

        if (!_name.empty()) builder.append(name(), _name);
        if (_ping.millis > 0ULL) builder.append(ping(), _ping);
        if (_up > 0) builder.append(up(), _up);
        if (_waiting) builder.append(waiting(), _waiting);

        return builder.obj();
    }

    bool MongosType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extract(source, name, "", &_name, errMsg)) return false;
        if (!FieldParser::extract(source, ping, 0ULL, &_ping, errMsg)) return false;
        if (!FieldParser::extract(source, up, 0, &_up, errMsg)) return false;
        if (!FieldParser::extract(source, waiting, false, &_waiting, errMsg)) return false;

        return true;
    }

    void MongosType::clear() {
        _name.clear();
        _ping = 0ULL;
        _up = 0;
        _waiting = false;
    }

    void MongosType::cloneTo(MongosType* other) const {
        other->clear();
        other->_name = _name;
        other->_ping = _ping;
        other->_up = _up;
        other->_waiting = _waiting;
    }

} // namespace mongo
