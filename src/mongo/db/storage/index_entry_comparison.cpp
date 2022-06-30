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
#include "mongo/platform/basic.h"

#include "mongo/db/storage/index_entry_comparison.h"

#include <ostream>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/hex.h"
#include "mongo/util/text.h"

namespace mongo {

std::ostream& operator<<(std::ostream& stream, const IndexKeyEntry& entry) {
    return stream << entry.key << '@' << entry.loc;
}

// Due to the limitations of various APIs, we need to use the same type (IndexKeyEntry)
// for both the stored data and the "query". We cheat and encode extra information in the
// first byte of the field names in the query. This works because all stored objects should
// have all field names empty, so their first bytes are '\0'.
enum BehaviorIfFieldIsEqual {
    normal = '\0',
    less = 'l',
    greater = 'g',
};

bool IndexEntryComparison::operator()(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const {
    // implementing in memcmp style to ease reuse of this code.
    return compare(lhs, rhs) < 0;
}

// This should behave the same as customBSONCmp from btree_logic.cpp.
//
// Reading the comment in the .h file is highly recommended if you need to understand what this
// function is doing
int IndexEntryComparison::compare(const IndexKeyEntry& lhs, const IndexKeyEntry& rhs) const {
    BSONObjIterator lhsIt(lhs.key);
    BSONObjIterator rhsIt(rhs.key);

    // Iterate through both BSONObjects, comparing individual elements one by one
    for (unsigned mask = 1; lhsIt.more(); mask <<= 1) {
        if (!rhsIt.more())
            return _order.descending(mask) ? -1 : 1;

        const BSONElement l = lhsIt.next();
        const BSONElement r = rhsIt.next();

        if (int cmp = l.woCompare(r, /*compareFieldNames=*/false)) {
            if (cmp == std::numeric_limits<int>::min()) {
                // can't be negated
                cmp = -1;
            }

            return _order.descending(mask) ? -cmp : cmp;
        }

        // Here is where the weirdness begins. We sometimes want to fudge the comparison
        // when a key == the query to implement exclusive ranges.
        BehaviorIfFieldIsEqual lEqBehavior = BehaviorIfFieldIsEqual(l.fieldName()[0]);
        BehaviorIfFieldIsEqual rEqBehavior = BehaviorIfFieldIsEqual(r.fieldName()[0]);

        if (lEqBehavior) {
            // lhs is the query, rhs is the stored data
            invariant(rEqBehavior == normal);
            return lEqBehavior == less ? -1 : 1;
        }

        if (rEqBehavior) {
            // rhs is the query, lhs is the stored data, so reverse the returns
            invariant(lEqBehavior == normal);
            return rEqBehavior == less ? 1 : -1;
        }
    }

    if (rhsIt.more())
        return -1;

    // This means just look at the key, not the loc.
    if (lhs.loc.isNull() || rhs.loc.isNull())
        return 0;

    return lhs.loc.compare(rhs.loc);  // is supposed to ignore ordering
}

KeyString::Value IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
    const IndexSeekPoint& seekPoint, KeyString::Version version, Ordering ord, bool isForward) {
    const bool inclusive = seekPoint.firstExclusive < 0;
    const auto discriminator = isForward == inclusive ? KeyString::Discriminator::kExclusiveBefore
                                                      : KeyString::Discriminator::kExclusiveAfter;

    KeyString::Builder builder(version, ord, discriminator);

    // Appends keyPrefix elements to the builder.
    if (seekPoint.prefixLen > 0) {
        BSONObjIterator it(seekPoint.keyPrefix);
        for (int i = 0; i < seekPoint.prefixLen; i++) {
            invariant(it.more());
            const BSONElement e = it.next();
            builder.appendBSONElement(e);
        }
    }

    // Handles the suffix. Note that the useful parts of the suffix start at index prefixLen rather
    // than at 0.
    size_t end = seekPoint.firstExclusive >= 0 ? static_cast<size_t>(seekPoint.firstExclusive + 1)
                                               : seekPoint.keySuffix.size();
    for (size_t i = seekPoint.prefixLen; i < end; i++) {
        invariant(seekPoint.keySuffix[i]);
        builder.appendBSONElement(*seekPoint.keySuffix[i]);
    }
    return builder.getValueCopy();
}

KeyString::Value IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(const BSONObj& bsonKey,
                                                                       KeyString::Version version,
                                                                       Ordering ord,
                                                                       bool isForward,
                                                                       bool inclusive) {
    return makeKeyStringFromBSONKey(bsonKey,
                                    version,
                                    ord,
                                    isForward == inclusive
                                        ? KeyString::Discriminator::kExclusiveBefore
                                        : KeyString::Discriminator::kExclusiveAfter);
}

KeyString::Value IndexEntryComparison::makeKeyStringFromBSONKey(const BSONObj& bsonKey,
                                                                KeyString::Version version,
                                                                Ordering ord,
                                                                KeyString::Discriminator discrim) {
    BSONObj finalKey = BSONObj::stripFieldNames(bsonKey);
    KeyString::Builder builder(version, finalKey, ord, discrim);
    return builder.getValueCopy();
}

Status buildDupKeyErrorStatus(const BSONObj& key,
                              const NamespaceString& collectionNamespace,
                              const std::string& indexName,
                              const BSONObj& keyPattern,
                              const BSONObj& indexCollation,
                              DuplicateKeyErrorInfo::FoundValue&& foundValue) {
    const bool hasCollation = !indexCollation.isEmpty();

    StringBuilder sb;
    sb << "E11000 duplicate key error";
    sb << " collection: " << collectionNamespace;
    if (indexName.size()) {
        // This helper may be used for clustered collections when there is no index for the cluster
        // key.
        sb << " index: " << indexName;
    }
    if (hasCollation) {
        sb << " collation: " << indexCollation;
    }
    sb << " dup key: ";

    // For the purpose of producing a useful error message, generate a representation of the key
    // with field names hydrated and with invalid UTF-8 hex-encoded.
    BSONObjBuilder builderForErrmsg;

    // Used to build a version of the key after hydrating with field names but without hex encoding
    // invalid UTF-8. This key is attached to the extra error info and consumed by callers who may
    // wish to retry on duplicate key errors. The field names are rehydrated so that we don't return
    // BSON with duplicate key names to clients.
    BSONObjBuilder builderForErrorExtraInfo;

    // key is a document with forms like: '{ : 123}', '{ : {num: 123} }', '{ : 123, : "str" }'
    BSONObjIterator keyValueIt(key);
    // keyPattern is a document with only one level. e.g. '{a : 1, b : -1}', '{a.b : 1}'
    BSONObjIterator keyNameIt(keyPattern);
    // Combine key and keyPattern into one document which represents a mapping from indexFieldName
    // to indexKey.
    while (1) {
        BSONElement keyValueElem = keyValueIt.next();
        BSONElement keyNameElem = keyNameIt.next();
        if (keyNameElem.eoo())
            break;

        builderForErrorExtraInfo.appendAs(keyValueElem, keyNameElem.fieldName());

        // If the duplicate key value contains a string, then it's possible that the string contains
        // binary data which is not valid UTF-8. This is true for all indexes with a collation,
        // since the index stores collation keys rather than raw user strings. But it's also
        // possible that the application has stored binary data inside a string, which the system
        // has never rejected.
        //
        // If the string in the key is invalid UTF-8, then we hex encode it before adding it to the
        // error message so that the driver can assume valid UTF-8 when reading the reply.
        const bool shouldHexEncode = keyValueElem.type() == BSONType::String &&
            (hasCollation || !isValidUTF8(keyValueElem.valueStringData()));

        if (shouldHexEncode) {
            auto stringToEncode = keyValueElem.valueStringData();
            builderForErrmsg.append(keyNameElem.fieldName(),
                                    "0x" + hexblob::encodeLower(stringToEncode));
        } else {
            builderForErrmsg.appendAs(keyValueElem, keyNameElem.fieldName());
        }
    }

    sb << builderForErrmsg.obj();

    stdx::visit(
        OverloadedVisitor{
            [](stdx::monostate) {},
            [&sb](const RecordId& rid) { sb << " found value: " << rid; },
            [&sb](const BSONObj& obj) {
                if (obj.objsize() < BSONObjMaxUserSize / 2) {
                    sb << " found value: " << obj;
                }
            },
        },
        foundValue);

    return Status(
        DuplicateKeyErrorInfo(
            keyPattern, builderForErrorExtraInfo.obj(), indexCollation, std::move(foundValue)),
        sb.str());
}

Status buildDupKeyErrorStatus(const KeyString::Value& keyString,
                              const NamespaceString& collectionNamespace,
                              const std::string& indexName,
                              const BSONObj& keyPattern,
                              const BSONObj& indexCollation,
                              const Ordering& ordering) {
    const BSONObj key = KeyString::toBson(
        keyString.getBuffer(), keyString.getSize(), ordering, keyString.getTypeBits());

    return buildDupKeyErrorStatus(key, collectionNamespace, indexName, keyPattern, indexCollation);
}

Status buildDupKeyErrorStatus(OperationContext* opCtx,
                              const KeyString::Value& keyString,
                              const Ordering& ordering,
                              const IndexDescriptor* desc) {
    const BSONObj key = KeyString::toBson(
        keyString.getBuffer(), keyString.getSize(), ordering, keyString.getTypeBits());
    return buildDupKeyErrorStatus(opCtx, key, desc);
}

Status buildDupKeyErrorStatus(OperationContext* opCtx,
                              const BSONObj& key,
                              const IndexDescriptor* desc) {
    NamespaceString nss;
    // In testing these may be nullptr, and being a bit more lenient during error handling is OK.
    if (desc && desc->getEntry())
        nss = desc->getEntry()->getNSSFromCatalog(opCtx);
    return buildDupKeyErrorStatus(
        key, nss, desc->indexName(), desc->keyPattern(), desc->collation());
}
}  // namespace mongo
