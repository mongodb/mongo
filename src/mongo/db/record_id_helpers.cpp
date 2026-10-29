// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/record_id_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace record_id_helpers {

StatusWith<RecordId> keyForOptime(const Timestamp& opTime, const KeyFormat keyFormat) {
    switch (keyFormat) {
        case KeyFormat::Long: {
            // Make sure secs and inc wouldn't be negative if treated as signed. This ensures that
            // they don't sort differently when put in a RecordId. It also avoids issues with
            // Null/Invalid RecordIds
            if (opTime.getSecs() > uint32_t(std::numeric_limits<int32_t>::max()))
                return {ErrorCodes::BadValue, "ts secs too high"};

            if (opTime.getInc() > uint32_t(std::numeric_limits<int32_t>::max()))
                return {ErrorCodes::BadValue, "ts inc too high"};

            auto out = RecordId(opTime.getSecs(), opTime.getInc());
            if (out <= RecordId::minLong())
                return {ErrorCodes::BadValue, "ts too low"};
            if (out >= RecordId::maxLong())
                return {ErrorCodes::BadValue, "ts too high"};
            return {std::move(out)};
        }
        case KeyFormat::String: {
            key_string::Builder keyBuilder(key_string::Version::kLatestVersion);
            keyBuilder.appendTimestamp(opTime);
            return RecordId(keyBuilder.getView());
        }
        default: {
            MONGO_UNREACHABLE_TASSERT(6521004);
        }
    }

    MONGO_UNREACHABLE_TASSERT(6521005);
}


/**
 * data and len must be the arguments from RecordStore::insert() on an oplog collection.
 */
StatusWith<RecordId> extractKeyOptime(const char* data, int len) {
    // Use the latest BSON validation version. Oplog entries are allowed to contain decimal data
    // even if decimal is disabled.
    if (kDebugBuild) {
        invariantStatusOK(validateBSON(data, len));
    }

    const BSONObj obj(data);
    const BSONElement elem = obj["ts"];
    if (elem.eoo())
        return {ErrorCodes::BadValue, "no ts field"};
    if (elem.type() != BSONType::timestamp)
        return {ErrorCodes::BadValue, "ts must be a Timestamp"};

    return keyForOptime(elem.timestamp(), KeyFormat::Long);
}

StatusWith<RecordId> keyForDoc(const BSONObj& doc,
                               const ClusteredIndexSpec& indexSpec,
                               const CollatorInterface* collator) {
    // Get the collection's cluster key field name
    const auto clusterKeyField = clustered_util::getClusterKeyFieldName(indexSpec);
    // Build a RecordId using the cluster key.
    const BSONElement keyElement = doc.getField(clusterKeyField);
    if (keyElement.eoo()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Document " << redact(doc) << " is missing the '"
                              << clusterKeyField << "' field"};
    }
    if (collator) {
        BSONObjBuilder out;
        CollationIndexKey::collationAwareIndexKeyAppend(keyElement, collator, &out);
        return keyForElem(out.done().firstElement());
    }

    return keyForElem(keyElement);
}

RecordId keyForElem(const BSONElement& elem) {
    // Intentionally discard the TypeBits since the type information will be stored in the cluster
    // key of the original document. The consequence of this behavior is that cluster key values
    // that compare similarly, but are of different types may not be used concurrently.
    key_string::Builder keyBuilder(key_string::Version::kLatestVersion);
    keyBuilder.appendBSONElement(elem);
    return RecordId(keyBuilder.getView());
}

RecordId keyForObj(const BSONObj& obj) {
    return keyForElem(obj.firstElement());
}

RecordId keyForOID(OID oid) {
    key_string::Builder keyBuilder(key_string::Version::kLatestVersion);
    keyBuilder.appendOID(oid);
    return RecordId(keyBuilder.getView());
}

RecordId keyForDate(Date_t date) {
    key_string::Builder keyBuilder(key_string::Version::kLatestVersion);
    keyBuilder.appendDate(date);
    return RecordId(keyBuilder.getView());
}

void appendToBSONAs(const RecordId& rid, BSONObjBuilder* builder, std::string_view fieldName) {
    rid.withFormat([&](RecordId::Null) { builder->appendNull(fieldName); },
                   [&](int64_t val) { builder->append(fieldName, val); },
                   [&](const char* str, int len) {
                       key_string::appendSingleFieldToBSONAs(str, len, fieldName, builder);
                   });
}

BSONObj toBSONAs(const RecordId& rid, std::string_view fieldName) {
    BSONObjBuilder builder;
    appendToBSONAs(rid, &builder, fieldName);
    return builder.obj();
}

namespace {
static constexpr int64_t kMinReservedLong = RecordId::kMaxRepr - (1024 * 1024);
// All RecordId strings that start with FF are considered reserved. This also happens to be an
// invalid start byte for a KeyString sequence, which is used to encode RecordId binary strings.
static constexpr char kReservedStrPrefix = static_cast<char>(0xFF);
}  // namespace

RecordId reservedIdFor(ReservationId res, KeyFormat keyFormat) {
    // There is only one reservation at the moment.
    invariant(res == ReservationId::kWildcardMultikeyMetadataId);
    if (keyFormat == KeyFormat::Long) {
        return RecordId(kMinReservedLong);
    } else {
        invariant(keyFormat == KeyFormat::String);
        constexpr char reservation[] = {
            kReservedStrPrefix, static_cast<char>(ReservationId::kWildcardMultikeyMetadataId)};
        return RecordId(reservation);
    }
}

RecordId maxRecordId(KeyFormat keyFormat) {
    if (keyFormat == KeyFormat::Long) {
        return RecordId::maxLong();
    } else {
        invariant(keyFormat == KeyFormat::String);
        constexpr char reservation[] = {
            kReservedStrPrefix, static_cast<char>(ReservationId::kWildcardMultikeyMetadataId)};
        return RecordId(reservation);
    }
}

bool isReserved(const RecordId& id) {
    if (id.isNull()) {
        return false;
    }
    if (id.isLong()) {
        return id.getLong() >= kMinReservedLong && id.getLong() < RecordId::kMaxRepr;
    }
    // All RecordId strings that start with FF are considered reserved.
    return id.getStr()[0] == kReservedStrPrefix;
}

}  // namespace record_id_helpers
}  // namespace mongo
