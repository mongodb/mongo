// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] record_id_helpers {

/**
 * Converts Timestamp to a RecordId in an unspecified manor that is safe to use as the key to
 * in a RecordStore.
 */
StatusWith<RecordId> keyForOptime(const Timestamp& opTime, KeyFormat keyFormat);

/**
 * For clustered collections, converts various values into a RecordId.
 */
StatusWith<RecordId> keyForDoc(const BSONObj& doc,
                               const ClusteredIndexSpec& indexSpec,
                               const CollatorInterface* collator);
RecordId keyForElem(const BSONElement& elem);
RecordId keyForObj(const BSONObj& obj);
RecordId keyForOID(OID oid);
RecordId keyForDate(Date_t date);

/**
 * data and len must be the arguments from RecordStore::insert() on an oplog collection.
 */
StatusWith<RecordId> extractKeyOptime(const char* data, int len);

/**
 * Helpers to append RecordIds to a BSON object builder. Note that this resolves the underlying BSON
 * type of the RecordId if it stores a KeyString.
 *
 * This should be used for informational purposes only. This cannot be 'round-tripped' back into a
 * RecordId because it loses information about the original RecordId format. If you require passing
 * a RecordId as a token or storing for a resumable scan, for example, use RecordId::serializeToken.
 */
void appendToBSONAs(const RecordId& rid, BSONObjBuilder* builder, std::string_view fieldName);
BSONObj toBSONAs(const RecordId& rid, std::string_view fieldName);

/**
 * Enumerates all reserved ids that have been allocated for a specific purpose. These IDs may not be
 * stored in RecordStores, but rather may be encoded as RecordIds as meaningful values in indexes.
 */
enum class ReservationId { kWildcardMultikeyMetadataId = 0 };

/**
 * Returns the reserved RecordId value for a given ReservationId and RecordStore KeyFormat.
 */
RecordId reservedIdFor(ReservationId res, KeyFormat keyFormat);

/**
 * Returns the maximum RecordId value for a given RecordStore KeyFormat.
 */
RecordId maxRecordId(KeyFormat keyFormat);

/**
 * Returns true if this RecordId falls within the reserved range for a given RecordId type.
 */
bool isReserved(const RecordId& id);

}  // namespace record_id_helpers
}  // namespace mongo
