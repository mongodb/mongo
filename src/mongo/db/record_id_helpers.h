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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_format.h"

namespace mongo {
class Timestamp;
class RecordId;

namespace record_id_helpers {

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
void appendToBSONAs(const RecordId& rid, BSONObjBuilder* builder, StringData fieldName);
BSONObj toBSONAs(const RecordId& rid, StringData fieldName);

/**
 * Enumerates all reserved ids that have been allocated for a specific purpose. These IDs may not be
 * stored in RecordStores, but rather may be encoded as RecordIds as meaningful values in indexes.
 */
enum class ReservationId { kWildcardMultikeyMetadataId };

/**
 * Returns the reserved RecordId value for a given ReservationId and RecordStore KeyFormat.
 */
RecordId reservedIdFor(ReservationId res, KeyFormat keyFormat);

/**
 * Returns true if this RecordId falls within the reserved range for a given RecordId type.
 */
bool isReserved(const RecordId& id);

}  // namespace record_id_helpers
}  // namespace mongo
