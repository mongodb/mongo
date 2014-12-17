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
#include "mongo/s/type_database.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string DatabaseType::ConfigNS = "config.databases";

    const BSONField<std::string> DatabaseType::name("_id");
    const BSONField<std::string> DatabaseType::primary("primary");
    const BSONField<bool> DatabaseType::draining("draining", false);
    const BSONField<bool> DatabaseType::DEPRECATED_partitioned("partitioned");
    const BSONField<std::string> DatabaseType::DEPRECATED_name("name");
    const BSONField<bool> DatabaseType::DEPRECATED_sharded("sharded");

    DatabaseType::DatabaseType() {
        clear();
    }

    DatabaseType::~DatabaseType() {
    }

    bool DatabaseType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isNameSet) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        if (!_isPrimarySet) {
            *errMsg = stream() << "missing " << primary.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj DatabaseType::toBSON() const {
        BSONObjBuilder builder;

        if (_isNameSet) builder.append(name(), _name);
        if (_isPrimarySet) builder.append(primary(), _primary);
        if (_isDrainingSet) builder.append(draining(), _draining);

        return builder.obj();
    }

    bool DatabaseType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, name, &_name, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNameSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, primary, &_primary, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isPrimarySet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, draining, &_draining, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isDrainingSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void DatabaseType::clear() {

        _name.clear();
        _isNameSet = false;

        _primary.clear();
        _isPrimarySet = false;

        _draining = false;
        _isDrainingSet = false;

    }

    void DatabaseType::cloneTo(DatabaseType* other) const {
        other->clear();

        other->_name = _name;
        other->_isNameSet = _isNameSet;

        other->_primary = _primary;
        other->_isPrimarySet = _isPrimarySet;

        other->_draining = _draining;
        other->_isDrainingSet = _isDrainingSet;

    }

    std::string DatabaseType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
