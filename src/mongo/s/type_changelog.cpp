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
#include "mongo/s/type_changelog.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string ChangelogType::ConfigNS = "config.changelog";

    const BSONField<std::string> ChangelogType::changeID("_id");
    const BSONField<std::string> ChangelogType::server("server");
    const BSONField<std::string> ChangelogType::clientAddr("clientAddr");
    const BSONField<Date_t> ChangelogType::time("time");
    const BSONField<std::string> ChangelogType::what("what");
    const BSONField<std::string> ChangelogType::ns("ns");
    const BSONField<BSONObj> ChangelogType::details("details");

    ChangelogType::ChangelogType() {
        clear();
    }

    ChangelogType::~ChangelogType() {
    }

    bool ChangelogType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isChangeIDSet) {
            *errMsg = stream() << "missing " << changeID.name() << " field";
            return false;
        }
        if (!_isServerSet) {
            *errMsg = stream() << "missing " << server.name() << " field";
            return false;
        }
        if (!_isClientAddrSet) {
            *errMsg = stream() << "missing " << clientAddr.name() << " field";
            return false;
        }
        if (!_isTimeSet) {
            *errMsg = stream() << "missing " << time.name() << " field";
            return false;
        }
        if (!_isWhatSet) {
            *errMsg = stream() << "missing " << what.name() << " field";
            return false;
        }
        if (!_isNsSet) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (!_isDetailsSet) {
            *errMsg = stream() << "missing " << details.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj ChangelogType::toBSON() const {
        BSONObjBuilder builder;

        if (_isChangeIDSet) builder.append(changeID(), _changeID);
        if (_isServerSet) builder.append(server(), _server);
        if (_isClientAddrSet) builder.append(clientAddr(), _clientAddr);
        if (_isTimeSet) builder.append(time(), _time);
        if (_isWhatSet) builder.append(what(), _what);
        if (_isNsSet) builder.append(ns(), _ns);
        if (_isDetailsSet) builder.append(details(), _details);

        return builder.obj();
    }

    bool ChangelogType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, changeID, "", &_changeID, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isChangeIDSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, server, "", &_server, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isServerSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, clientAddr, "", &_clientAddr, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isClientAddrSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, time, 0, &_time, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isTimeSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, what, "", &_what, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isWhatSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, ns, "", &_ns, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, details, BSONObj(), &_details, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isDetailsSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void ChangelogType::clear() {

        _changeID.clear();
        _isChangeIDSet = false;

        _server.clear();
        _isServerSet = false;

        _clientAddr.clear();
        _isClientAddrSet = false;

        _time = 0ULL;
        _isTimeSet = false;

        _what.clear();
        _isWhatSet = false;

        _ns.clear();
        _isNsSet = false;

        _details = BSONObj();
        _isDetailsSet = false;

    }

    void ChangelogType::cloneTo(ChangelogType* other) const {
        other->clear();

        other->_changeID = _changeID;
        other->_isChangeIDSet = _isChangeIDSet;

        other->_server = _server;
        other->_isServerSet = _isServerSet;

        other->_clientAddr = _clientAddr;
        other->_isClientAddrSet = _isClientAddrSet;

        other->_time = _time;
        other->_isTimeSet = _isTimeSet;

        other->_what = _what;
        other->_isWhatSet = _isWhatSet;

        other->_ns = _ns;
        other->_isNsSet = _isNsSet;

        other->_details = _details;
        other->_isDetailsSet = _isDetailsSet;

    }

    std::string ChangelogType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
