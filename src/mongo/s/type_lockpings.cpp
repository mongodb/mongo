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

#include "mongo/s/type_lockpings.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string LockpingsType::ConfigNS = "config.lockpings";

    BSONField<string> LockpingsType::process("_id");
    BSONField<Date_t> LockpingsType::ping("ping");

    LockpingsType::LockpingsType() {
        clear();
    }

    LockpingsType::~LockpingsType() {
    }

    bool LockpingsType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        if (_process.empty()) {
            *errMsg = stream() << "missing " << process.name() << " field";
            return false;
        }
        if (_ping.millis == 0) {
            *errMsg = stream() << "missing " << ping.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj LockpingsType::toBSON() const {
        BSONObjBuilder builder;

        if (!_process.empty()) builder.append(process(), _process);
        if (_ping.millis > 0ULL) builder.append(ping(), _ping);

        return builder.obj();
    }

    void LockpingsType::parseBSON(BSONObj source) {
        clear();

        bool ok = true;
        ok &= FieldParser::extract(source, process, "", &_process);
        ok &= FieldParser::extract(source, ping, 0ULL, &_ping);
        if (! ok) {
            clear();
        }
    }

    void LockpingsType::clear() {
        _process.clear();
        _ping = 0ULL;
    }

    void LockpingsType::cloneTo(LockpingsType* other) {
        other->clear();
        other->_process = _process;
        other->_ping = _ping;
    }

} // namespace mongo
