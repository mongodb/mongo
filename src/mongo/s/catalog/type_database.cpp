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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_database.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::string;

const std::string DatabaseType::ConfigNS = "config.databases";

const BSONField<std::string> DatabaseType::name("_id");
const BSONField<std::string> DatabaseType::primary("primary");
const BSONField<bool> DatabaseType::sharded("partitioned");


StatusWith<DatabaseType> DatabaseType::fromBSON(const BSONObj& source) {
    DatabaseType dbt;

    {
        std::string dbtName;
        Status status = bsonExtractStringField(source, name.name(), &dbtName);
        if (!status.isOK())
            return status;

        dbt._name = dbtName;
    }

    {
        std::string dbtPrimary;
        Status status = bsonExtractStringField(source, primary.name(), &dbtPrimary);
        if (!status.isOK())
            return status;

        dbt._primary = dbtPrimary;
    }

    {
        bool dbtSharded;
        Status status =
            bsonExtractBooleanFieldWithDefault(source, sharded.name(), false, &dbtSharded);
        if (!status.isOK())
            return status;

        dbt._sharded = dbtSharded;
    }

    return StatusWith<DatabaseType>(dbt);
}

Status DatabaseType::validate() const {
    if (!_name.is_initialized() || _name->empty()) {
        return Status(ErrorCodes::NoSuchKey, "missing name");
    }

    if (!_primary.is_initialized() || !_primary->isValid()) {
        return Status(ErrorCodes::NoSuchKey, "missing primary");
    }

    if (!_sharded.is_initialized()) {
        return Status(ErrorCodes::NoSuchKey, "missing sharded");
    }

    return Status::OK();
}

BSONObj DatabaseType::toBSON() const {
    BSONObjBuilder builder;
    builder.append(name.name(), _name.get_value_or(""));
    builder.append(primary.name(), _primary.get_value_or(ShardId()).toString());
    builder.append(sharded.name(), _sharded.get_value_or(false));

    return builder.obj();
}

std::string DatabaseType::toString() const {
    return toBSON().toString();
}

void DatabaseType::setName(const std::string& name) {
    invariant(!name.empty());
    _name = name;
}

void DatabaseType::setPrimary(const ShardId& primary) {
    invariant(primary.isValid());
    _primary = primary;
}

}  // namespace mongo
