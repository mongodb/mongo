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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kVersion[] = "version";
const char kLastmod[] = "lastmod";

}  // namespace

const char ChunkVersion::kShardVersionField[] = "shardVersion";

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONForCommands(const BSONObj& obj) {
    return parseFromBSONWithFieldForCommands(obj, kShardVersionField);
}

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONWithFieldForCommands(const BSONObj& obj,
                                                                         StringData field) {
    BSONElement versionElem;
    Status status = bsonExtractField(obj, field, &versionElem);
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

StatusWith<ChunkVersion> ChunkVersion::parseFromBSONForChunk(const BSONObj& obj) {
    bool canParse;
    const ChunkVersion chunkVersion = ChunkVersion::fromBSON(obj, kLastmod, &canParse);
    if (!canParse)
        return {ErrorCodes::BadValue, "Unable to parse shard version"};

    return chunkVersion;
}

void ChunkVersion::appendForSetShardVersion(BSONObjBuilder* builder) const {
    addToBSON(*builder, kVersion);
}

void ChunkVersion::appendForCommands(BSONObjBuilder* builder) const {
    appendWithFieldForCommands(builder, kShardVersionField);
}

void ChunkVersion::appendWithFieldForCommands(BSONObjBuilder* builder, StringData field) const {
    builder->appendArray(field, toBSON());
}

void ChunkVersion::appendForChunk(BSONObjBuilder* builder) const {
    addToBSON(*builder, kLastmod);
}

BSONObj ChunkVersion::toBSON() const {
    BSONArrayBuilder b;
    b.appendTimestamp(_combined);
    b.append(_epoch);
    return b.arr();
}

}  // namespace mongo
