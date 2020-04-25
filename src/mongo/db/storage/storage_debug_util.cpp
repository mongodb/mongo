/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_debug_util.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace StorageDebugUtil {

void printKeyString(const RecordId& recordId,
                    const KeyString::Value& keyStringValue,
                    const BSONObj& keyPatternBson,
                    const BSONObj& keyStringBson,
                    std::string callerLogPrefix) {
    // We need to rehydrate the keyString to something readable.
    auto keyPatternIter = keyPatternBson.begin();
    auto keyStringIter = keyStringBson.begin();
    BSONObjBuilder b;
    while (keyPatternIter != keyPatternBson.end() && keyStringIter != keyStringBson.end()) {
        b.appendAs(*keyStringIter, keyPatternIter->fieldName());
        ++keyPatternIter;
        ++keyStringIter;
    }
    // Wildcard index documents can have more values in the keystring.
    while (keyStringIter != keyStringBson.end()) {
        b.append(*keyStringIter);
        ++keyStringIter;
    }
    BSONObj rehydratedKey = b.done();

    LOGV2(51811,
          "{caller} {record_id}, key: {rehydrated_key}, keystring: "
          "{key_string}",
          "caller"_attr = callerLogPrefix,
          "record_id"_attr = recordId,
          "rehydrated_key"_attr = rehydratedKey,
          "key_string"_attr = keyStringValue);
}

void printCollectionAndIndexTableEntries(OperationContext* opCtx, const NamespaceString& nss) {
    invariant(!opCtx->lockState()->isLocked());
    AutoGetCollection autoColl(opCtx, nss, MODE_IS);
    Collection* coll = autoColl.getCollection();

    LOGV2(51807, "Dumping collection table and index tables' entries for debugging...");

    // Iterate and print the collection table (record store) documents.
    RecordStore* rs = coll->getRecordStore();
    auto rsCursor = rs->getCursor(opCtx);
    boost::optional<Record> rec = rsCursor->next();
    LOGV2(51808, "[Debugging] Collection table entries:");
    while (rec) {
        LOGV2(51809,
              "[Debugging](record) {record_id}, Value: {record_data}",
              "record_id"_attr = rec->id,
              "record_data"_attr = rec->data.toBson());
        rec = rsCursor->next();
    }

    // Iterate and print each index's table of documents.
    const auto indexCatalog = coll->getIndexCatalog();
    const auto it = indexCatalog->getIndexIterator(opCtx, /*includeUnfinished*/ false);
    while (it->more()) {
        const auto indexCatalogEntry = it->next();
        const auto indexDescriptor = indexCatalogEntry->descriptor();
        const auto iam = indexCatalogEntry->accessMethod();
        auto indexCursor = iam->newCursor(opCtx, /*forward*/ true);

        const BSONObj& keyPattern = indexDescriptor->keyPattern();
        const KeyString::Version version = iam->getSortedDataInterface()->getKeyStringVersion();
        const auto ordering = Ordering::make(keyPattern);
        KeyString::Builder firstKeyString(
            version, BSONObj(), ordering, KeyString::Discriminator::kExclusiveBefore);

        LOGV2(51810,
              "[Debugging] {keyPattern_str} index table entries:",
              "keyPattern_str"_attr = keyPattern);

        for (auto keyStringEntry = indexCursor->seekForKeyString(firstKeyString.getValueCopy());
             keyStringEntry;
             keyStringEntry = indexCursor->nextKeyString()) {
            auto keyString = KeyString::toBsonSafe(keyStringEntry->keyString.getBuffer(),
                                                   keyStringEntry->keyString.getSize(),
                                                   ordering,
                                                   keyStringEntry->keyString.getTypeBits());
            printKeyString(keyStringEntry->loc,
                           keyStringEntry->keyString,
                           keyPattern,
                           keyString,
                           "[Debugging](index)");
        }
    }
}

void printValidateResults(const ValidateResults& results) {
    std::stringstream ss;

    ss << "ValidateResults:\nValid: " << results.valid << "\n"
       << "Errors:\n";

    for (const std::string& error : results.errors) {
        ss << "\t" << error << "\n";
    }

    ss << "Warnings:\n";
    for (const std::string& warning : results.warnings) {
        ss << "\t" << warning << "\n";
    }

    ss << "Extra index entries:\n";
    for (const BSONObj& obj : results.extraIndexEntries) {
        ss << "\t" << obj << "\n";
    }

    ss << "Missing index entries:\n";
    for (const BSONObj& obj : results.missingIndexEntries) {
        ss << "\t" << obj << "\n";
    }

    LOGV2(51812, "{results_string}", "results_string"_attr = ss.str());
}

}  // namespace StorageDebugUtil

}  // namespace mongo
