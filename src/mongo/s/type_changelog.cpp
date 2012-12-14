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
    BSONField<std::string> ChangelogType::changeID("_id");
    BSONField<std::string> ChangelogType::server("server");
    BSONField<std::string> ChangelogType::clientAddr("clientAddr");
    BSONField<Date_t> ChangelogType::time("time");
    BSONField<std::string> ChangelogType::what("what");
    BSONField<std::string> ChangelogType::ns("ns");
    BSONField<BSONObj> ChangelogType::details("details");

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
        if (_changeID.empty()) {
            *errMsg = stream() << "missing " << changeID.name() << " field";
            return false;
        }
        if (_server.empty()) {
            *errMsg = stream() << "missing " << server.name() << " field";
            return false;
        }
        if (_clientAddr.empty()) {
            *errMsg = stream() << "missing " << clientAddr.name() << " field";
            return false;
        }
        if (_time.millis == 0) {
            *errMsg = stream() << "missing " << time.name() << " field";
            return false;
        }
        if (_what.empty()) {
            *errMsg = stream() << "missing " << what.name() << " field";
            return false;
        }
        if (_ns.empty()) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (_details.nFields() == 0) {
            *errMsg = stream() << "missing " << details.name() << " field";
            return false;
        }
        return true;
    }

    BSONObj ChangelogType::toBSON() const {
        BSONObjBuilder builder;
        if (!_changeID.empty()) builder.append(changeID(), _changeID);
        if (!_server.empty()) builder.append(server(), _server);
        if (!_clientAddr.empty()) builder.append(clientAddr(), _clientAddr);
        if (_time.millis > 0ULL) builder.append(time(), _time);
        if (!_what.empty()) builder.append(what(), _what);
        if (!_ns.empty()) builder.append(ns(), _ns);
        if (_details.nFields()) builder.append(details(), _details);
        return builder.obj();
    }

    bool ChangelogType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extract(source, changeID, "", &_changeID, errMsg)) return false;
        if (!FieldParser::extract(source, server, "", &_server, errMsg)) return false;
        if (!FieldParser::extract(source, clientAddr, "", &_clientAddr, errMsg)) return false;
        if (!FieldParser::extract(source, time, 0ULL, &_time, errMsg)) return false;
        if (!FieldParser::extract(source, what, "", &_what, errMsg)) return false;
        if (!FieldParser::extract(source, ns, "", &_ns, errMsg)) return false;
        if (!FieldParser::extract(source, details, BSONObj(), &_details, errMsg)) return false;

        return true;
    }

    void ChangelogType::clear() {
        _changeID.clear();
        _server.clear();
        _clientAddr.clear();
        _time = 0ULL;
        _what.clear();
        _ns.clear();
        _details = BSONObj();
    }

    void ChangelogType::cloneTo(ChangelogType* other) {
        other->clear();
        other->_changeID = _changeID;
        other->_server = _server;
        other->_clientAddr = _clientAddr;
        other->_time = _time;
        other->_what = _what;
        other->_ns = _ns;
        other->_details = _details;
    }

    std::string ChangelogType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
