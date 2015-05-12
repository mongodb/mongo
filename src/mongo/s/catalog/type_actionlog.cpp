/**
 *    Copyright (C) 2014 MongoDB Inc.
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
#include "mongo/s/catalog/type_actionlog.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::string;

    using mongo::str::stream;

    const std::string ActionLogType::ConfigNS = "config.actionlog";

    const BSONField<std::string> ActionLogType::server("server");
    const BSONField<std::string> ActionLogType::what("what");
    const BSONField<Date_t> ActionLogType::time("time");
    const BSONField<BSONObj> ActionLogType::details("details");

    ActionLogType::ActionLogType() {
        clear();
    }

    ActionLogType::~ActionLogType() {
    }

    bool ActionLogType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isServerSet) {
            *errMsg = stream() << "missing " << server.name() << " field";
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
        if (!_isDetailsSet) {
            *errMsg = stream() << "missing " << details.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj ActionLogType::toBSON() const {
        BSONObjBuilder builder;

        if (_isServerSet) builder.append(server(), _server);
        if (_isTimeSet) builder.append(time(), _time);
        if (_isWhatSet) builder.append(what(), _what);
        if (_isDetailsSet) builder.append(details(), _details);

        return builder.obj();
    }

    bool ActionLogType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;

        fieldState = FieldParser::extract(source, server, &_server, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isServerSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, time, &_time, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isTimeSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, what, &_what, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isWhatSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, details, &_details, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isDetailsSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void ActionLogType::clear() {

        _server.clear();
        _isServerSet = false;

        _what.clear();
        _isWhatSet = false;

        _time = 0ULL;
        _isTimeSet = false;

        _details = BSONObj();
        _isDetailsSet = false;

    }

    void ActionLogType::cloneTo(ActionLogType* other) const {
        other->clear();

        other->_server = _server;
        other->_isServerSet = _isServerSet;

        other->_what = _what;
        other->_isWhatSet = _isWhatSet;

        other->_time = _time;
        other->_isTimeSet = _isTimeSet;

        other->_details = _details;
        other->_isDetailsSet = _isDetailsSet;

    }

    std::string ActionLogType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
