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
#include "mongo/s/database_version_helpers.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::string;

const NamespaceString DatabaseType::ConfigNS("config.databases");

const BSONField<std::string> DatabaseType::name("_id");
const BSONField<std::string> DatabaseType::primary("primary");
const BSONField<bool> DatabaseType::sharded("partitioned");
const BSONField<BSONObj> DatabaseType::version("version");

DatabaseType::DatabaseType(const std::string& dbName,
                           const ShardId& primaryShard,
                           bool sharded,
                           DatabaseVersion version)
    : _name(dbName), _primary(primaryShard), _sharded(sharded), _version(version) {}

StatusWith<DatabaseType> DatabaseType::fromBSON(const BSONObj& source) {
    std::string dbtName;
    {
        Status status = bsonExtractStringField(source, name.name(), &dbtName);
        if (!status.isOK())
            return status;
    }

    std::string dbtPrimary;
    {
        Status status = bsonExtractStringField(source, primary.name(), &dbtPrimary);
        if (!status.isOK())
            return status;
    }

    bool dbtSharded;
    {
        Status status =
            bsonExtractBooleanFieldWithDefault(source, sharded.name(), false, &dbtSharded);
        if (!status.isOK())
            return status;
    }

    DatabaseVersion dbtVersion;
    {
        BSONObj versionField = source.getObjectField("version");
        if (versionField.isEmpty()) {
            return Status{ErrorCodes::InternalError,
                          str::stream() << "DatabaseVersion doesn't exist in database entry "
                                        << source
                                        << " despite the config server being in binary version 4.2 "
                                           "or later."};
        }
        dbtVersion = DatabaseVersion::parse(IDLParserErrorContext("DatabaseType"), versionField);
    }

    return DatabaseType{
        std::move(dbtName), std::move(dbtPrimary), dbtSharded, std::move(dbtVersion)};
}

Status DatabaseType::validate() const {
    if (_name.empty()) {
        return Status(ErrorCodes::NoSuchKey, "missing name");
    }

    if (!_primary.isValid()) {
        return Status(ErrorCodes::NoSuchKey, "missing primary");
    }

    return Status::OK();
}

BSONObj DatabaseType::toBSON() const {
    BSONObjBuilder builder;

    // Required fields.
    builder.append(name.name(), _name);
    builder.append(primary.name(), _primary.toString());
    builder.append(sharded.name(), _sharded);
    builder.append(version.name(), _version.toBSON());

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

void DatabaseType::setSharded(bool sharded) {
    _sharded = sharded;
}

void DatabaseType::setVersion(const DatabaseVersion& version) {
    _version = version;
}

}  // namespace mongo
