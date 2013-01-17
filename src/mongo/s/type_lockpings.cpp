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

    const BSONField<std::string> LockpingsType::process("_id");
    const BSONField<Date_t> LockpingsType::ping("ping");

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

        // All the mandatory fields must be present.
        if (!_isProcessSet) {
            *errMsg = stream() << "missing " << process.name() << " field";
            return false;
        }
        if (!_isPingSet) {
            *errMsg = stream() << "missing " << ping.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj LockpingsType::toBSON() const {
        BSONObjBuilder builder;

        if (_isProcessSet) builder.append(process(), _process);
        if (_isPingSet) builder.append(ping(), _ping);

        return builder.obj();
    }

    bool LockpingsType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, process, "", &_process, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isProcessSet = fieldState == FieldParser::FIELD_VALID;

        fieldState = FieldParser::extract(source, ping, 0, &_ping, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isPingSet = fieldState == FieldParser::FIELD_VALID;

        return true;
    }

    void LockpingsType::clear() {

        _process.clear();
        _isProcessSet = false;

        _ping = 0ULL;
        _isPingSet = false;

    }

    void LockpingsType::cloneTo(LockpingsType* other) const {
        other->clear();

        other->_process = _process;
        other->_isProcessSet = _isProcessSet;

        other->_ping = _ping;
        other->_isPingSet = _isPingSet;

    }

    std::string LockpingsType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
