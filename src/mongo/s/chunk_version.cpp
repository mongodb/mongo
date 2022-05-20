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
#include "mongo/s/pm2583_feature_flags_gen.h"
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

ChunkVersion ChunkVersion::_parseArrayOrObjectPositionalFormat(const BSONObj& obj) {
    BSONObjIterator it(obj);
    uassert(ErrorCodes::BadValue, "Unexpected empty version array", it.more());

    // Expect the major and minor versions (must be present)
    uint64_t combined;
    {
        BSONElement tsPart = it.next();
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Invalid type " << tsPart.type()
                              << " for version major and minor part.",
                tsPart.type() == bsonTimestamp);
        combined = tsPart.timestamp().asULL();
    }

    // Expect the epoch OID (must be present)
    boost::optional<OID> epoch;
    {
        BSONElement epochPart = it.next();
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Invalid type " << epochPart.type() << " for version epoch part.",
                epochPart.type() == jstOID);
        epoch = epochPart.OID();
    }

    BSONElement nextElem = it.next();

    // TODO SERVER-59105: remove once 6.0 is last-lts. For backward compatibility reasons 5.0
    // routers sends canThrowSSVOnIgnored even though it is not used, so we attempt to parse and
    // ignore it.
    if (nextElem.type() == BSONType::Bool) {
        nextElem = it.next();
    }

    // Check for timestamp
    boost::optional<Timestamp> timestamp;
    if (nextElem.type() == bsonTimestamp) {
        timestamp = nextElem.timestamp();
    } else if (nextElem.eoo() && (epoch == UNSHARDED().epoch() || epoch == IGNORED().epoch())) {
        // In 5.0 binaries, the timestamp is not present in UNSHARDED and IGNORED versions
        timestamp =
            (epoch == UNSHARDED().epoch() ? UNSHARDED().getTimestamp() : IGNORED().getTimestamp());
    } else {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "Invalid type " << nextElem.type()
                                << " for version timestamp part.");
    }

    ChunkVersion version;
    version._combined = combined;
    version._epoch = *epoch;
    version._timestamp = *timestamp;
    return version;
}

StatusWith<ChunkVersion> ChunkVersion::_parseLegacyWithField(const BSONObj& obj, StringData field) {
    // Expect the major and minor (must always exist)
    uint64_t combined;
    {
        auto versionElem = obj[field];
        if (versionElem.eoo())
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "Expected field " << field << " not found."};

        if (versionElem.type() == bsonTimestamp || versionElem.type() == Date) {
            combined = versionElem._numberLong();
        } else {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Invalid type " << versionElem.type()
                                  << " for version major and minor part."};
        }
    }

    // Expect the epoch OID
    //
    // TODO: Confirm whether the epoch can still be missing in upgrade chains that started from
    //       pre-2.4 versions anymore (after FCV 4.4 -> 5.0 upgrade) ?
    boost::optional<OID> epoch;
    {
        const auto epochField = field + "Epoch";
        auto epochElem = obj[epochField];
        if (epochElem.type() == jstOID) {
            epoch = epochElem.OID();
        } else if (!epochElem.eoo()) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Invalid type " << epochElem.type()
                                  << " for version epoch part."};
        }
    }

    // Expect the timestamp (can be missing only in the case of pre-5.0 UNSHARDED and IGNORED
    // versions)
    boost::optional<Timestamp> timestamp;
    {
        const auto timestampField = field + "Timestamp";
        auto timestampElem = obj[timestampField];
        if (timestampElem.type() == bsonTimestamp) {
            timestamp = timestampElem.timestamp();
        } else if (!timestampElem.eoo()) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "Invalid type " << timestampElem.type()
                                  << " for version timestamp part."};
        }
    }

    if (epoch && timestamp) {
        // Expected situation
    } else if (epoch && !timestamp) {
        if (epoch == UNSHARDED().epoch() || epoch == IGNORED().epoch()) {
            // In 5.0 binaries, the timestamp is not present in UNSHARDED and IGNORED versions
            timestamp = (epoch == UNSHARDED().epoch() ? UNSHARDED().getTimestamp()
                                                      : IGNORED().getTimestamp());
        } else {
            uasserted(6278300, "Timestamp must be present if epoch exists.");
        }
    } else if (!epoch && timestamp) {
        uasserted(6278301, "Epoch must be present if timestamp exists.");
    } else {
        // Can happen in upgrade chains that started from pre-2.4 versions or in the case of
        // persistence for ShardCollectionType
    }

    ChunkVersion version;
    version._combined = combined;
    version._epoch = epoch.value_or(OID());
    version._timestamp = timestamp.value_or(Timestamp());
    return version;
}

ChunkVersion ChunkVersion::fromBSONLegacyOrNewerFormat(const BSONObj& obj, StringData field) {
    // New format.
    if (obj[field].isABSONObj()) {
        return parse(obj[field]);
    }

    // Legacy format.
    return uassertStatusOK(ChunkVersion::_parseLegacyWithField(obj, field));
}

ChunkVersion ChunkVersion::fromBSONPositionalOrNewerFormat(const BSONElement& element) {
    auto obj = element.Obj();

    // Positional or wrongly encoded format.
    if (obj.couldBeArray()) {
        return ChunkVersion::_parseArrayOrObjectPositionalFormat(obj);
    }

    // New format.
    return parse(element);
}

ChunkVersion ChunkVersion::parse(const BSONElement& element) {
    auto parsedVersion =
        ChunkVersion60Format::parse(IDLParserErrorContext("ChunkVersion"), element.Obj());
    auto version = parsedVersion.getVersion();
    return ChunkVersion(version.getSecs(),
                        version.getInc(),
                        parsedVersion.getEpoch(),
                        parsedVersion.getTimestamp());
}

void ChunkVersion::serializeToBSON(StringData field, BSONObjBuilder* builder) const {
    ChunkVersion60Format version;
    version.setGeneration({_epoch, _timestamp});
    version.setPlacement(Timestamp(majorVersion(), minorVersion()));
    builder->append(field, version.toBSON());
}

void ChunkVersion::appendLegacyWithField(BSONObjBuilder* out, StringData field) const {
    if (feature_flags::gFeatureFlagNewPersistedChunkVersionFormat.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        ChunkVersion60Format version;
        version.setGeneration({_epoch, _timestamp});
        version.setPlacement(Timestamp(majorVersion(), minorVersion()));
        out->append(field, version.toBSON());
    } else {
        out->appendTimestamp(field, _combined);
        out->append(field + "Epoch", _epoch);
        out->append(field + "Timestamp", _timestamp);
    }
}

std::string ChunkVersion::toString() const {
    return str::stream() << majorVersion() << "|" << minorVersion() << "||" << _epoch << "||"
                         << _timestamp.toString();
}

ChunkVersion ChunkVersion::parseMajorMinorVersionOnlyFromShardCollectionType(
    const BSONElement& element) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Invalid type " << element.type()
                          << " for version major and minor part.",
            element.type() == bsonTimestamp || element.type() == Date);

    ChunkVersion version;
    version._combined = element._numberLong();
    return version;
}

void ChunkVersion::serialiseMajorMinorVersionOnlyForShardCollectionType(StringData field,
                                                                        BSONObjBuilder* out) const {
    out->appendTimestamp(field, toLong());
}

}  // namespace mongo
