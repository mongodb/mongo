/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/type_migration.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

const StringData kChunkVersion = "chunkVersion"_sd;

}  // namespace

const NamespaceString MigrationType::ConfigNS("config.migrations");

const BSONField<std::string> MigrationType::ns("ns");
const BSONField<BSONObj> MigrationType::min("min");
const BSONField<BSONObj> MigrationType::max("max");
const BSONField<std::string> MigrationType::fromShard("fromShard");
const BSONField<std::string> MigrationType::toShard("toShard");
const BSONField<bool> MigrationType::waitForDelete("waitForDelete");
const BSONField<std::string> MigrationType::forceJumbo("forceJumbo");

MigrationType::MigrationType() = default;

MigrationType::MigrationType(const MigrateInfo& info, bool waitForDelete)
    : _nss(info.nss),
      _min(info.minKey),
      _max(info.maxKey),
      _fromShard(info.from),
      _toShard(info.to),
      _chunkVersion(info.version),
      _waitForDelete(waitForDelete),
      _forceJumbo(MoveChunkRequest::forceJumboToString(info.forceJumbo)) {}

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
        migrationType._toShard = std::move(migrationToShard);
    }

    {
        std::string migrationFromShard;
        Status status = bsonExtractStringField(source, fromShard.name(), &migrationFromShard);
        if (!status.isOK())
            return status;
        migrationType._fromShard = std::move(migrationFromShard);
    }

    {
        auto chunkVersionStatus = ChunkVersion::parseWithField(source, kChunkVersion);
        if (!chunkVersionStatus.isOK())
            return chunkVersionStatus.getStatus();
        migrationType._chunkVersion = chunkVersionStatus.getValue();
    }

    {
        bool waitForDeleteVal{false};
        Status status = bsonExtractBooleanFieldWithDefault(
            source, waitForDelete.name(), false, &waitForDeleteVal);
        if (!status.isOK())
            return status;
        migrationType._waitForDelete = waitForDeleteVal;
    }

    {
        std::string forceJumboVal;
        Status status = bsonExtractStringField(source, forceJumbo.name(), &forceJumboVal);
        if (!status.isOK())
            return status;

        auto forceJumbo = MoveChunkRequest::parseForceJumbo(forceJumboVal);
        if (forceJumbo != MoveChunkRequest::ForceJumbo::kDoNotForce &&
            forceJumbo != MoveChunkRequest::ForceJumbo::kForceManual &&
            forceJumbo != MoveChunkRequest::ForceJumbo::kForceBalancer) {
            return Status{ErrorCodes::BadValue, "Unknown value for forceJumbo"};
        }
        migrationType._forceJumbo = std::move(forceJumboVal);
    }

    return migrationType;
}

BSONObj MigrationType::toBSON() const {
    BSONObjBuilder builder;

    builder.append(ns.name(), _nss.ns());

    builder.append(min.name(), _min);
    builder.append(max.name(), _max);

    builder.append(fromShard.name(), _fromShard.toString());
    builder.append(toShard.name(), _toShard.toString());

    _chunkVersion.appendWithField(&builder, kChunkVersion);

    builder.append(waitForDelete.name(), _waitForDelete);
    builder.append(forceJumbo.name(), _forceJumbo);
    return builder.obj();
}

MigrateInfo MigrationType::toMigrateInfo(const UUID& uuid) const {
    ChunkType chunk;
    chunk.setShard(_fromShard);
    chunk.setCollectionUUID(uuid);
    chunk.setMin(_min);
    chunk.setMax(_max);
    chunk.setVersion(_chunkVersion);

    return MigrateInfo(_toShard,
                       _nss,
                       chunk,
                       MoveChunkRequest::parseForceJumbo(_forceJumbo),
                       MigrateInfo::chunksImbalance);
}

}  // namespace mongo
