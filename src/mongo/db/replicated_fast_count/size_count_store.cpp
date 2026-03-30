/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/size_count_store.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

namespace mongo::replicated_fast_count {

boost::optional<SizeCountStore::Entry> SizeCountStore::read(OperationContext* opCtx,
                                                            UUID uuid) const {
    const auto acquisition = acquireFastCountCollectionForRead(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();
    const RecordId rid =
        record_id_helpers::keyForDoc(BSON("_id" << uuid),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr)
            .getValue();
    Snapshotted<BSONObj> document;
    if (!coll->findDoc(opCtx, rid, &document)) {
        return boost::none;
    }

    const BSONObj& data = document.value();
    return SizeCountStore::Entry{
        .timestamp = data.getField(kValidAsOfKey).timestamp(),
        .size = data.getField(kMetadataKey).Obj().getField(kSizeKey).Long(),
        .count = data.getField(kMetadataKey).Obj().getField(kCountKey).Long()};
}

void SizeCountStore::write(OperationContext* opCtx, UUID uuid, const Entry& entry) {
    const auto acquisition = acquireFastCountCollectionForWrite(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();
    const RecordId rid =
        record_id_helpers::keyForDoc(BSON("_id" << uuid),
                                     clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                     /*collator=*/nullptr)
            .getValue();

    const BSONObj newDoc = BSON("_id" << uuid << kValidAsOfKey << entry.timestamp << kMetadataKey
                                      << BSON(kCountKey << entry.count << kSizeKey << entry.size));

    Snapshotted<BSONObj> existingDoc;
    if (coll->findDoc(opCtx, rid, &existingDoc)) {
        const auto diff = doc_diff::computeOplogDiff(existingDoc.value(), newDoc, /*padding=*/0);
        invariant(diff.has_value(),
                  fmt::format("Expected computed diff to be smaller than the post-image: "
                              "pre={}, post={}",
                              existingDoc.value().toString(),
                              newDoc.toString()));
        if (!diff->isEmpty()) {
            CollectionUpdateArgs args(existingDoc.value());
            args.update = update_oplog_entry::makeDeltaOplogEntry(*diff);
            args.criteria = BSON("_id" << uuid);
            collection_internal::updateDocument(
                opCtx, coll, rid, existingDoc, newDoc, &args.update, nullptr, nullptr, &args);
        }
    } else {
        massertStatusOK(collection_internal::insertDocument(
            opCtx, coll, InsertStatement(newDoc), /*opDebug=*/nullptr));
    }
}
}  // namespace mongo::replicated_fast_count
