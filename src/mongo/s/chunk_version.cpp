/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_version.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kVersion[] = "version";
const char kShardVersion[] = "shardVersion";

}  // namespace

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONForCommands(const BSONObj& obj) {
    BSONElement versionElem;
    Status status = bsonExtractField(obj, kShardVersion, &versionElem);
    if (!status.isOK())
        return status;

    if (versionElem.type() != Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Invalid type " << versionElem.type()
                              << " for shardVersion element. Expected an array"};
    }

    BSONObjIterator it(versionElem.Obj());
    if (!it.more())
        return {ErrorCodes::BadValue, "Unexpected empty version"};

    ChunkVersion version;

    // Expect the timestamp
    {
        BSONElement tsPart = it.next();
        if (tsPart.type() != bsonTimestamp)
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Invalid type " << tsPart.type()
                                  << " for version timestamp part."};

        version._combined = tsPart.timestamp().asULL();
    }

    // Expect the epoch OID
    {
        BSONElement epochPart = it.next();
        if (epochPart.type() != jstOID)
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Invalid type " << epochPart.type()
                                  << " for version epoch part."};

        version._epoch = epochPart.OID();
    }

    return version;
}

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONForSetShardVersion(const BSONObj& obj) {
    bool canParse;
    const ChunkVersion chunkVersion = ChunkVersion::fromBSON(obj, kVersion, &canParse);
    if (!canParse)
        return {ErrorCodes::BadValue, "Unable to parse shard version"};

    return chunkVersion;
}


ChunkVersionAndOpTime::ChunkVersionAndOpTime(ChunkVersion chunkVersion)
    : _verAndOpT(chunkVersion) {}

ChunkVersionAndOpTime::ChunkVersionAndOpTime(ChunkVersion chunkVersion, repl::OpTime ts)
    : _verAndOpT(chunkVersion, ts) {}

StatusWith<ChunkVersionAndOpTime> ChunkVersionAndOpTime::parseFromBSONForCommands(
    const BSONObj& obj) {
    const auto chunkVersionStatus = ChunkVersion::parseFromBSONForCommands(obj);
    if (!chunkVersionStatus.isOK())
        return chunkVersionStatus.getStatus();

    const ChunkVersion& chunkVersion = chunkVersionStatus.getValue();

    const auto opTimeStatus = repl::OpTime::parseFromOplogEntry(obj);
    if (opTimeStatus.isOK()) {
        return ChunkVersionAndOpTime(chunkVersion, opTimeStatus.getValue());
    } else if (opTimeStatus == ErrorCodes::NoSuchKey) {
        return ChunkVersionAndOpTime(chunkVersion);
    }

    return opTimeStatus.getStatus();
}

StatusWith<ChunkVersionAndOpTime> ChunkVersionAndOpTime::parseFromBSONForSetShardVersion(
    const BSONObj& obj) {
    const auto chunkVersionStatus = ChunkVersion::parseFromBSONForSetShardVersion(obj);
    if (!chunkVersionStatus.isOK())
        return chunkVersionStatus.getStatus();

    const ChunkVersion& chunkVersion = chunkVersionStatus.getValue();

    const auto opTimeStatus = repl::OpTime::parseFromOplogEntry(obj);
    if (opTimeStatus.isOK()) {
        return ChunkVersionAndOpTime(chunkVersion, opTimeStatus.getValue());
    } else if (opTimeStatus == ErrorCodes::NoSuchKey) {
        return ChunkVersionAndOpTime(chunkVersion);
    }

    return opTimeStatus.getStatus();
}

void ChunkVersionAndOpTime::appendForSetShardVersion(BSONObjBuilder* builder) const {
    _verAndOpT.value.addToBSON(*builder, kVersion);
    builder->append("ts", _verAndOpT.opTime.getTimestamp());
    builder->append("t", _verAndOpT.opTime.getTerm());
}

void ChunkVersionAndOpTime::appendForCommands(BSONObjBuilder* builder) const {
    builder->appendArray(kShardVersion, _verAndOpT.value.toBSON());
    builder->append("ts", _verAndOpT.opTime.getTimestamp());
    builder->append("t", _verAndOpT.opTime.getTerm());
}

}  // namespace mongo
