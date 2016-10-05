/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/s/balancer/type_migration.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

const std::string MigrationType::ConfigNS = "config.migrations";

const BSONField<std::string> MigrationType::name("_id");
const BSONField<std::string> MigrationType::ns("ns");
const BSONField<BSONObj> MigrationType::min("min");
const BSONField<BSONObj> MigrationType::max("max");
const BSONField<std::string> MigrationType::fromShard("fromShard");
const BSONField<std::string> MigrationType::toShard("toShard");

MigrationType::MigrationType() = default;

MigrationType::MigrationType(MigrateInfo info)
    : _nss(NamespaceString(info.ns)),
      _min(info.minKey),
      _max(info.maxKey),
      _fromShard(info.from),
      _toShard(info.to) {}

StatusWith<MigrationType> MigrationType::fromBSON(const BSONObj& source) {
    MigrationType migrationType;

    {
        std::string migrationNS;
        Status status = bsonExtractStringField(source, ns.name(), &migrationNS);
        if (!status.isOK())
            return status;
        migrationType._nss = NamespaceString(migrationNS);
    }

    {
        auto chunkRangeStatus = ChunkRange::fromBSON(source);
        if (!chunkRangeStatus.isOK())
            return chunkRangeStatus.getStatus();

        const auto chunkRange = std::move(chunkRangeStatus.getValue());
        migrationType._min = chunkRange.getMin().getOwned();
        migrationType._max = chunkRange.getMax().getOwned();
    }

    {
        std::string migrationToShard;
        Status status = bsonExtractStringField(source, toShard.name(), &migrationToShard);
        if (!status.isOK())
            return status;
        migrationType._toShard = migrationToShard;
    }

    {
        std::string migrationFromShard;
        Status status = bsonExtractStringField(source, fromShard.name(), &migrationFromShard);
        if (!status.isOK())
            return status;
        migrationType._fromShard = migrationFromShard;
    }

    return migrationType;
}

BSONObj MigrationType::toBSON() const {
    BSONObjBuilder builder;
    if (_nss && _min)
        builder.append(name.name(), getName());
    if (_nss)
        builder.append(ns.name(), _nss->ns());
    if (_min)
        builder.append(min.name(), _min.get());
    if (_max)
        builder.append(max.name(), _max.get());
    if (_fromShard)
        builder.append(fromShard.name(), _fromShard->toString());
    if (_toShard)
        builder.append(toShard.name(), _toShard->toString());

    return builder.obj();
}

MigrateInfo MigrationType::toMigrateInfo() const {
    return MigrateInfo(_nss->ns(), _toShard.get(), _fromShard.get(), _min.get(), _max.get());
}

std::string MigrationType::getName() const {
    return ChunkType::genID(_nss->ns(), _min.get());
}

}  // namespace mongo
