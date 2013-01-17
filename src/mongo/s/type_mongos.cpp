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

    const BSONField<std::string> MongosType::name("_id");
    const BSONField<Date_t> MongosType::ping("ping");
    const BSONField<int> MongosType::up("up");
    const BSONField<bool> MongosType::waiting("waiting");
    const BSONField<std::string> MongosType::mongoVersion("mongoVersion");
    const BSONField<int> MongosType::configVersion("configVersion");

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

        // All the mandatory fields must be present.
        if (!_isNameSet) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        if (!_isPingSet) {
            *errMsg = stream() << "missing " << ping.name() << " field";
            return false;
        }
        if (!_isUpSet) {
            *errMsg = stream() << "missing " << up.name() << " field";
            return false;
        }
        if (!_isWaitingSet) {
            *errMsg = stream() << "missing " << waiting.name() << " field";
            return false;
        }
        if (!_isMongoVersionSet) {
            *errMsg = stream() << "missing " << mongoVersion.name() << " field";
            return false;
        }
        if (!_isConfigVersionSet) {
            *errMsg = stream() << "missing " << configVersion.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj MongosType::toBSON() const {
        BSONObjBuilder builder;

        if (_isNameSet) builder.append(name(), _name);
        if (_isPingSet) builder.append(ping(), _ping);
        if (_isUpSet) builder.append(up(), _up);
        if (_isWaitingSet) builder.append(waiting(), _waiting);
        if (_isMongoVersionSet) builder.append(mongoVersion(), _mongoVersion);
        if (_isConfigVersionSet) builder.append(configVersion(), _configVersion);

        return builder.obj();
    }

    bool MongosType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, name, "", &_name, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNameSet = fieldState == FieldParser::FIELD_VALID;

        fieldState = FieldParser::extract(source, ping, 0, &_ping, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isPingSet = fieldState == FieldParser::FIELD_VALID;

        fieldState = FieldParser::extract(source, up, 0, &_up, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpSet = fieldState == FieldParser::FIELD_VALID;

        fieldState = FieldParser::extract(source, waiting, false, &_waiting, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isWaitingSet = fieldState == FieldParser::FIELD_VALID;

        fieldState = FieldParser::extract(source, mongoVersion, "", &_mongoVersion, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMongoVersionSet = fieldState == FieldParser::FIELD_VALID;

        fieldState = FieldParser::extract(source, configVersion, 0, &_configVersion, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isConfigVersionSet = fieldState == FieldParser::FIELD_VALID;

        return true;
    }

    void MongosType::clear() {

        _name.clear();
        _isNameSet = false;

        _ping = 0ULL;
        _isPingSet = false;

        _up = 0;
        _isUpSet = false;

        _waiting = false;
        _isWaitingSet = false;

        _mongoVersion.clear();
        _isMongoVersionSet = false;

        _configVersion = 0;
        _isConfigVersionSet = false;

    }

    void MongosType::cloneTo(MongosType* other) const {
        other->clear();

        other->_name = _name;
        other->_isNameSet = _isNameSet;

        other->_ping = _ping;
        other->_isPingSet = _isPingSet;

        other->_up = _up;
        other->_isUpSet = _isUpSet;

        other->_waiting = _waiting;
        other->_isWaitingSet = _isWaitingSet;

        other->_mongoVersion = _mongoVersion;
        other->_isMongoVersionSet = _isMongoVersionSet;

        other->_configVersion = _configVersion;
        other->_isConfigVersionSet = _isConfigVersionSet;

    }

    std::string MongosType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
