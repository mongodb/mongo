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

#include "mongo/s/chunk_version.h"

#include "mongo/s/chunk_version_gen.h"
#include "mongo/util/str.h"

namespace mongo {

constexpr StringData ChunkVersion::kShardVersionField;

bool CollectionGeneration::isSameCollection(const CollectionGeneration& other) const {
    if (_timestamp == other._timestamp) {
        tassert(664720,
                str::stream() << "Collections have matching timestamps " << _timestamp
                              << ", but different epochs " << _epoch << " vs " << other._epoch,
                _epoch == other._epoch);
        return true;
    }

    tassert(664721,
            str::stream() << "Collections have different timestamps " << _timestamp << " vs "
                          << other._timestamp << ", but matching epochs " << _epoch,
            _epoch != other._epoch);
    return false;
}

std::string CollectionGeneration::toString() const {
    return str::stream() << _epoch << "|" << _timestamp;
}

ChunkVersion ChunkVersion::parse(const BSONElement& element) {
    auto parsedVersion =
        ChunkVersion60Format::parse(IDLParserContext("ChunkVersion"), element.Obj());
    auto version = parsedVersion.getVersion();
    return ChunkVersion({parsedVersion.getEpoch(), parsedVersion.getTimestamp()},
                        {version.getSecs(), version.getInc()});
}

void ChunkVersion::serializeToBSON(StringData field, BSONObjBuilder* builder) const {
    ChunkVersion60Format version;
    version.setGeneration({_epoch, _timestamp});
    version.setPlacement(Timestamp(majorVersion(), minorVersion()));
    builder->append(field, version.toBSON());
}

std::string ChunkVersion::toString() const {
    return str::stream() << majorVersion() << "|" << minorVersion() << "||" << _epoch << "||"
                         << _timestamp.toString();
}

}  // namespace mongo
