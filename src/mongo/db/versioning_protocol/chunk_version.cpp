// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/versioning_protocol/chunk_version.h"

#include "mongo/db/versioning_protocol/chunk_version_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

constexpr std::string_view ChunkVersion::kChunkVersionField;

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
        ChunkVersion60Format::parse(element.Obj(), IDLParserContext("ChunkVersion"));
    auto version = parsedVersion.getVersion();
    return ChunkVersion({parsedVersion.getEpoch(), parsedVersion.getTimestamp()},
                        {version.getSecs(), version.getInc()});
}

void ChunkVersion::serialize(std::string_view field, BSONObjBuilder* builder) const {
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
