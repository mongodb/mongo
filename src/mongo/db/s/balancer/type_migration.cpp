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

#include "mongo/idl/idl_parser.h"
#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/type_migration.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_range_request_gen.h"

namespace mongo {

const NamespaceString MigrationType::ConfigNS("config.migrations");

const BSONField<std::string> MigrationType::ns("ns");
const BSONField<BSONObj> MigrationType::min("min");
const BSONField<BSONObj> MigrationType::max("max");
const BSONField<std::string> MigrationType::fromShard("fromShard");
const BSONField<std::string> MigrationType::toShard("toShard");
const BSONField<bool> MigrationType::waitForDelete("waitForDelete");
const BSONField<std::string> MigrationType::forceJumbo("forceJumbo");
const BSONField<std::string> MigrationType::chunkVersion("chunkVersion");
const BSONField<int64_t> MigrationType::maxChunkSizeBytes("maxChunkSizeBytes");


MigrationType::MigrationType() = default;

MigrationType::MigrationType(
    const NamespaceString& nss,
    const BSONObj& min,
    const BSONObj& max,
    const ShardId& fromShard,
    const ShardId& toShard,
    const ChunkVersion& chunkVersion,
    bool waitForDelete,
    ForceJumbo forceJumbo,
    const boost::optional<int64_t>& maxChunkSizeBytes,
    const boost::optional<MigrationSecondaryThrottleOptions>& secondaryThrottle)
    : _nss(nss),
      _min(min),
      _max(max),
      _fromShard(fromShard),
      _toShard(toShard),
      _chunkVersion(chunkVersion),
      _waitForDelete(waitForDelete),
      _forceJumbo(forceJumbo),
      _maxChunkSizeBytes(maxChunkSizeBytes),
      _secondaryThrottle(secondaryThrottle) {}

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

    try {
        auto chunkVersionStatus = ChunkVersion::parse(source[chunkVersion.name()]);
        migrationType._chunkVersion = chunkVersionStatus;
    } catch (const DBException& ex) {
        return ex.toStatus();
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
        long long forceJumboVal;
        Status status = bsonExtractIntegerField(source, forceJumbo.name(), &forceJumboVal);
        if (!status.isOK())
            return status;

        migrationType._forceJumbo = ForceJumbo_parse(IDLParserErrorContext("ForceJumbo"),
                                                     static_cast<int32_t>(forceJumboVal));
    }

    {
        long long maxChunkSizeBytesVal;
        Status status =
            bsonExtractIntegerField(source, maxChunkSizeBytes.name(), &maxChunkSizeBytesVal);
        if (status.isOK()) {
            migrationType._maxChunkSizeBytes = maxChunkSizeBytesVal;
        } else {
            migrationType._maxChunkSizeBytes = boost::none;
        }
    }

    {
        auto swSecondaryThrottle = MigrationSecondaryThrottleOptions::createFromCommand(source);
        if (swSecondaryThrottle.getStatus().isOK()) {
            migrationType._secondaryThrottle = swSecondaryThrottle.getValue();
        } else {
            migrationType._secondaryThrottle = boost::none;
        }
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

    _chunkVersion.serializeToBSON(chunkVersion.name(), &builder);

    builder.append(waitForDelete.name(), _waitForDelete);
    builder.append(forceJumbo.name(), _forceJumbo);
    if (_maxChunkSizeBytes.is_initialized()) {
        builder.appendNumber(maxChunkSizeBytes.name(), static_cast<long long>(*_maxChunkSizeBytes));
    }
    if (_secondaryThrottle.is_initialized()) {
        _secondaryThrottle->append(&builder);
    }
    return builder.obj();
}

}  // namespace mongo
