// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/set_multikey_metadata_oplog_helpers.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"

namespace mongo::set_multikey_metadata_oplog_helpers {

std::vector<std::string> extractFieldPathsFromMetadataKeys(const KeyStringSet& metadataKeys,
                                                           const Ordering& ordering) {
    std::vector<std::string> fieldPaths;
    fieldPaths.reserve(metadataKeys.size());

    for (const auto& ks : metadataKeys) {
        auto bsonKey = key_string::toBson(ks, ordering);

        BSONObjIterator iter(bsonKey);
        bool foundMarker = false;
        while (iter.more()) {
            auto elem = iter.next();
            if (elem.type() != BSONType::minKey) {
                tassert(11609101,
                        "Expected integer 1 marker in wildcard metadata key",
                        elem.isNumber() && elem.numberInt() == 1);
                tassert(11609102,
                        "Expected field path string after marker in wildcard metadata key",
                        iter.more());
                auto pathElem = iter.next();
                tassert(11609103,
                        "Expected string type for field path in wildcard metadata key",
                        pathElem.type() == BSONType::string);
                fieldPaths.emplace_back(pathElem.valueStringData());
                foundMarker = true;
                break;
            }
        }
        tassert(11609104,
                "Wildcard metadata key contained no non-MinKey marker; cannot extract field path",
                foundMarker);
    }
    return fieldPaths;
}

BSONObj fieldPathsToBSON(const std::vector<std::string>& fieldPaths) {
    BSONArrayBuilder arrBuilder;
    for (const auto& path : fieldPaths) {
        arrBuilder.append(path);
    }
    return arrBuilder.arr();
}

/*
 * Regeneration — for each path string:
 *   1. Count 'prefixFields' and 'suffixFields' around the "$**" position in 'keyPattern'.
 *      E.g. {x: 1, $**: 1, y: 1, z: 1} → prefix=1, suffix=2.
 *   2. Build a KeyString matching the encoding written by the primary's `WildcardKeyGenerator`:
 *         MinKey × prefixFields, integer 1, "<path>", MinKey × suffixFields, reservedRecordId.
 *      Delegates to `WildcardKeyGenerator::makeMultikeyMetadataKey` so the encoding stays
 *      shared between primary (doc-walk path) and secondary (oplog-apply path).
 */
KeyStringSet regenerateMetadataKeysFromFieldPaths(const BSONObj& pathsObj,
                                                  key_string::Version version,
                                                  Ordering ordering,
                                                  KeyFormat rsKeyFormat,
                                                  const BSONObj& keyPattern) {
    size_t prefixFields = 0;
    size_t suffixFields = 0;
    bool foundWildcard = false;
    for (const auto& field : keyPattern) {
        if (WildcardNames::isWildcardFieldName(field.fieldNameStringData())) {
            tassert(11609106,
                    str::stream() << "keyPattern contains multiple wildcard fields: " << keyPattern,
                    !foundWildcard);
            foundWildcard = true;
        } else if (foundWildcard) {
            ++suffixFields;
        } else {
            ++prefixFields;
        }
    }
    tassert(11609107,
            str::stream() << "Expected wildcard ($**) field in keyPattern: " << keyPattern,
            foundWildcard);

    KeyStringSet result;
    SharedBufferFragmentBuilder pooledBufferBuilder(
        key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    for (const auto& elem : pathsObj) {
        tassert(11609108,
                str::stream() << "Expected string elements in wildcard multikey paths array in "
                                 "setMultikeyMetadata oplog entry, got type "
                              << typeName(elem.type()),
                elem.type() == BSONType::string);
        result.insert(WildcardKeyGenerator::makeMultikeyMetadataKey(elem.valueStringData(),
                                                                    prefixFields,
                                                                    suffixFields,
                                                                    version,
                                                                    ordering,
                                                                    rsKeyFormat,
                                                                    pooledBufferBuilder));
    }
    return result;
}

}  // namespace mongo::set_multikey_metadata_oplog_helpers
