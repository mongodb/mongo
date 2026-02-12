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

#include "mongo/db/global_catalog/shard_key_pattern.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/path_internal.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

constexpr auto kIdField = "_id"_sd;

const BSONObj kNullObj = BSON("" << BSONNULL);

/**
 * Currently the allowable shard keys are either:
 * i) a single field, e.g. { a : "hashed" }, {a: 1} or
 * ii) a compound list of ascending, potentially-nested field paths, e.g. { a : 1 , b.c : 1 }
 * iii) a compound hashed shard key with exactly one hashed field e.g. {a: 1, b: 'hashed', c: 1}
 */
std::vector<std::unique_ptr<FieldRef>> parseShardKeyPattern(const BSONObj& keyPattern) {
    uassert(ErrorCodes::BadValue, "Shard key is empty", !keyPattern.isEmpty());

    std::vector<std::unique_ptr<FieldRef>> parsedPaths;

    auto numHashedFields = 0;
    for (const auto& patternEl : keyPattern) {
        auto newFieldRef(std::make_unique<FieldRef>(patternEl.fieldNameStringData()));

        // Empty path
        uassert(ErrorCodes::BadValue,
                str::stream() << "Field " << patternEl.fieldNameStringData() << " is empty",
                newFieldRef->numParts() > 0);

        // Extra "." in path?
        uassert(ErrorCodes::BadValue,
                str::stream() << "Field " << patternEl.fieldNameStringData()
                              << " contains extra dot",
                newFieldRef->dottedField() == patternEl.fieldNameStringData());

        // Empty parts of the path, ".."?
        for (size_t i = 0; i < newFieldRef->numParts(); ++i) {
            const StringData part = newFieldRef->getPart(i);

            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field " << patternEl.fieldNameStringData()
                                  << " contains empty parts",
                    !part.empty());

            // Reject a shard key that has a field name that starts with '$' or contains parts that
            // start with '$' unless the part is a DBRef (i.e. is equal to '$id', '$db' or '$ref').
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field " << patternEl.fieldNameStringData()
                                  << " contains parts that start with '$'",
                    !part.starts_with("$") ||
                        (i != 0 && (part == "$db" || part == "$id" || part == "$ref")));
        }

        // Numeric and ascending (1.0), or "hashed" with exactly hashed field.
        auto isHashedPattern = ShardKeyPattern::isHashedPatternEl(patternEl);
        if (isHashedPattern) {
            numHashedFields += 1;
            uassert(
                ErrorCodes::BadValue,
                str::stream() << "Shard key " << keyPattern.toString()
                              << " can contain at most one 'hashed' field. Failed to parse field "
                              << patternEl.fieldNameStringData(),
                numHashedFields == 1);
        } else {
            uassert(
                ErrorCodes::BadValue,
                str::stream() << "Shard key " << keyPattern.toString()
                              << "numerical fields must be set to a value of 1 (ascending). Failed "
                                 "to parse field "
                              << patternEl.fieldNameStringData(),
                (patternEl.isNumber() && patternEl.safeNumberInt() == 1));
        }
        parsedPaths.emplace_back(std::move(newFieldRef));
    }

    return parsedPaths;
}

bool isValidShardKeyElementForExtractionFromDocument(const BSONElement& element) {
    return element.type() != BSONType::array;
}

bool isValidShardKeyElement(const BSONElement& element) {
    return !element.eoo() && element.type() != BSONType::array;
}

BSONElement extractKeyElementFromDoc(const BSONObj& obj, StringData pathStr) {
    // Any arrays found get immediately returned. We are equipped up the call stack to specifically
    // deal with array values.
    size_t idxPath;
    return getFieldDottedOrArray(obj, FieldRef(pathStr), &idxPath);
}

/**
 * Extracts the BSONElement matching 'fieldName' from the 'indexKeyDataVector'. Returns a pair with
 * first field representing the matching BSONElement and the second field representing whether the
 * value is hashed or not. In cases where there is more than one match for 'fieldName' we return the
 * first matching non-hashed value.
 */
std::pair<BSONElement, bool> extractFieldFromIndexData(
    const std::vector<ShardKeyPattern::IndexKeyData>& indexKeyDataVector, StringData fieldName) {
    std::pair<BSONElement, bool> output;
    for (auto&& indexKeyData : indexKeyDataVector) {
        BSONObjIterator keyDataIt(indexKeyData.data);
        for (auto&& keyPatternElt : indexKeyData.pattern) {
            invariant(keyDataIt.more());
            BSONElement keyDataElt = keyDataIt.next();
            if (fieldName == keyPatternElt.fieldNameStringData()) {
                const auto isHashed = (keyPatternElt.valueStringData() == IndexNames::HASHED);
                output = {keyDataElt, isHashed};
                if (!isHashed) {
                    return output;
                }
                // If the field is hashed, do not return immediately. We will continue to look for
                // raw document value in other indexes.
                break;
            }
        }
    }
    return output;
}

BSONElement extractFieldFromDocumentKey(const BSONObj& documentKey, StringData fieldName) {
    BSONElement output;
    for (auto&& documentKeyElt : documentKey) {
        if (fieldName == documentKeyElt.fieldNameStringData()) {
            return documentKeyElt;
        }
    }
    return output;
}

}  // namespace

Status ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(const BSONObj& shardKey) {
    for (const auto& elem : shardKey) {
        if (!isValidShardKeyElementForStorage(elem)) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Shard key element " << elem << " is not valid for storage"};
        }
    }

    return Status::OK();
}

BSONElement ShardKeyPattern::extractHashedField(BSONObj keyPattern) {
    for (auto&& element : keyPattern) {
        if (isHashedPatternEl(element)) {
            return element;
        }
    }
    return BSONElement();
}
ShardKeyPattern::ShardKeyPattern(const BSONObj& keyPattern)
    : _keyPattern(keyPattern),
      _keyPatternPaths(parseShardKeyPattern(keyPattern)),
      _hasId(keyPattern.hasField("_id"_sd)),
      _hashedField(extractHashedField(keyPattern)) {}

ShardKeyPattern::ShardKeyPattern(const KeyPattern& keyPattern)
    : ShardKeyPattern(keyPattern.toBSON()) {}

bool ShardKeyPattern::isHashedPatternEl(const BSONElement& el) {
    return el.type() == BSONType::string && el.String() == IndexNames::HASHED;
}

bool ShardKeyPattern::isHashedPattern() const {
    return !_hashedField.eoo();
}

bool ShardKeyPattern::isHashedOnField(StringData fieldName) const {
    return !_hashedField.eoo() && _hashedField.fieldNameStringData() == fieldName;
}

bool ShardKeyPattern::isValidHashedValue(const BSONElement& el) {
    switch (el.type()) {
        case BSONType::minKey:
        case BSONType::maxKey:
        case BSONType::numberLong:
            return true;
        default:
            return false;
    }
}


bool ShardKeyPattern::hasHashedPrefix() const {
    return isHashedPatternEl(_keyPattern.toBSON().firstElement());
}

const BSONObj& ShardKeyPattern::toBSON() const {
    return _keyPattern.toBSON();
}

std::string ShardKeyPattern::toString() const {
    return toBSON().toString();
}

std::string ShardKeyPattern::toKeyString(const BSONObj& shardKey) {
    key_string::Builder ks(key_string::Version::V1, Ordering::allAscending());

    BSONObjIterator it(shardKey);
    while (auto elem = it.next()) {
        ks.appendBSONElement(elem);
    }

    return {ks.getView().data(), ks.getView().size()};
}

bool ShardKeyPattern::isShardKey(const BSONObj& shardKey) const {
    const auto& keyPatternBSON = _keyPattern.toBSON();

    for (const auto& patternEl : keyPatternBSON) {
        BSONElement keyEl = shardKey[patternEl.fieldNameStringData()];

        if (!isValidShardKeyElement(keyEl))
            return false;
    }

    return shardKey.nFields() == keyPatternBSON.nFields();
}

bool ShardKeyPattern::isExtendedBy(const ShardKeyPattern& newShardKeyPattern) const {
    return toBSON().isPrefixOf(newShardKeyPattern.toBSON(), SimpleBSONElementComparator::kInstance);
}

BSONObj ShardKeyPattern::normalizeShardKey(const BSONObj& shardKey) const {
    // We want to return an empty key if users pass us something that's not a shard key
    if (shardKey.nFields() > _keyPattern.toBSON().nFields())
        return BSONObj();

    BSONObjBuilder keyBuilder;
    BSONObjIterator patternIt(_keyPattern.toBSON());
    while (patternIt.more()) {
        BSONElement patternEl = patternIt.next();

        BSONElement keyEl = shardKey[patternEl.fieldNameStringData()];

        if (!isValidShardKeyElement(keyEl))
            return BSONObj();

        keyBuilder.appendAs(keyEl, patternEl.fieldName());
    }

    dassert(isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

BSONObj ShardKeyPattern::extractShardKeyFromIndexKeyData(
    const std::vector<IndexKeyData>& indexKeyDataVector) const {
    BSONObjBuilder keyBuilder;
    for (auto&& shardKeyField : _keyPattern.toBSON()) {
        auto [matchEl, isAlreadyHashed] =
            extractFieldFromIndexData(indexKeyDataVector, shardKeyField.fieldNameStringData());
        invariant(matchEl);

        // A shard key field cannot have array values. If we encounter array values return
        // immediately.
        if (!isValidShardKeyElementForExtractionFromDocument(matchEl)) {
            return BSONObj();
        }

        // There are four possible cases here:
        // 1. Index provides hashed data and the shard key field is hashed. Then we append the
        // data as it is.
        // 2. Index provides actual data and the shard key field is hashed. Then we hash the data
        // before appending.
        // 3. Index provides actual data and the shard key field is non-hashed. Then we append the
        // data as it is.
        // 4. Index provides hashed data and the shard key field is non-hashed. This can never
        // happen and we should invariant.
        if (isAlreadyHashed) {
            invariant(isHashedPatternEl(shardKeyField));
        }
        if (!isAlreadyHashed && isHashedPatternEl(shardKeyField)) {
            keyBuilder.append(
                shardKeyField.fieldNameStringData(),
                BSONElementHasher::hash64(matchEl, BSONElementHasher::DEFAULT_HASH_SEED));
        } else {
            // NOTE: The matched element may *not* have the same field name as the path -
            // index keys don't contain field names, for example.
            keyBuilder.appendAs(matchEl, shardKeyField.fieldNameStringData());
        }
    }
    dassert(isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

BSONObj ShardKeyPattern::extractShardKeyFromDocumentKey(const BSONObj& documentKey) const {
    BSONObjBuilder keyBuilder;
    for (auto&& shardKeyField : _keyPattern.toBSON()) {
        auto matchEl =
            extractFieldFromDocumentKey(documentKey, shardKeyField.fieldNameStringData());

        if (matchEl.eoo()) {
            matchEl = kNullObj.firstElement();
        }

        // A shard key field cannot have array values. If we encounter array values return
        // immediately.
        if (!isValidShardKeyElementForExtractionFromDocument(matchEl)) {
            return BSONObj();
        }

        if (isHashedPatternEl(shardKeyField)) {
            keyBuilder.append(
                shardKeyField.fieldNameStringData(),
                BSONElementHasher::hash64(matchEl, BSONElementHasher::DEFAULT_HASH_SEED));
        } else {
            keyBuilder.appendAs(matchEl, shardKeyField.fieldNameStringData());
        }
    }
    dassert(isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

BSONObj ShardKeyPattern::extractShardKeyFromDocumentKeyThrows(const BSONObj& documentKey) const {
    auto shardKey = extractShardKeyFromDocumentKey(documentKey);

    uassert(ErrorCodes::ShardKeyNotFound,
            "Shard key cannot contain array values or array descendants.",
            !shardKey.isEmpty());

    return shardKey;
}

BSONObj ShardKeyPattern::extractShardKeyFromDoc(const BSONObj& doc) const {
    BSONObjBuilder keyBuilder;
    for (auto&& patternEl : _keyPattern.toBSON()) {
        BSONElement matchEl = extractKeyElementFromDoc(doc, patternEl.fieldNameStringData());

        if (matchEl.eoo()) {
            matchEl = kNullObj.firstElement();
        }

        if (!isValidShardKeyElementForExtractionFromDocument(matchEl)) {
            return BSONObj();
        }

        if (isHashedPatternEl(patternEl)) {
            keyBuilder.append(
                patternEl.fieldName(),
                BSONElementHasher::hash64(matchEl, BSONElementHasher::DEFAULT_HASH_SEED));
        } else {
            // NOTE: The matched element may *not* have the same field name as the path -
            // index keys don't contain field names, for example
            keyBuilder.appendAs(matchEl, patternEl.fieldName());
        }
    }

    dassert(isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

BSONObj ShardKeyPattern::extractShardKeyFromDocThrows(const BSONObj& doc) const {
    auto shardKey = extractShardKeyFromDoc(doc);

    uassert(ErrorCodes::ShardKeyNotFound,
            "Shard key cannot contain array values or array descendants.",
            !shardKey.isEmpty());

    return shardKey;
}

BSONObj ShardKeyPattern::emplaceMissingShardKeyValuesForDocument(const BSONObj doc) const {
    BSONObjBuilder fullDocBuilder(doc);
    for (const auto& skField : _keyPattern.toBSON()) {
        // Illegal to emplace a null _id.
        if (skField.fieldNameStringData() == kIdField) {
            continue;
        }
        auto matchEl = extractKeyElementFromDoc(doc, skField.fieldNameStringData());
        if (matchEl.eoo()) {
            fullDocBuilder << skField.fieldNameStringData() << BSONNULL;
        }
    }

    return fullDocBuilder.obj();
}

bool ShardKeyPattern::isIndexUniquenessCompatible(const BSONObj& indexPattern) const {
    if (!indexPattern.isEmpty() && indexPattern.firstElementFieldName() == kIdField) {
        return true;
    }

    return _keyPattern.toBSON().isFieldNamePrefixOf(indexPattern);
}

size_t ShardKeyPattern::getApproximateSize() const {
    auto computeVectorSize = [](const std::vector<std::unique_ptr<FieldRef>>& v) {
        size_t size = 0;
        for (const auto& ptr : v) {
            size += sizeof(ptr) + (ptr ? ptr->estimateObjectSizeInBytes() : 0);
        }
        return size;
    };

    auto size = sizeof(ShardKeyPattern);
    size += _keyPattern.getApproximateSize() - sizeof(KeyPattern);
    size += computeVectorSize(_keyPatternPaths);
    return size;
}

bool ShardKeyPattern::isValidShardKeyElementForStorage(const BSONElement& element) {
    if (!isValidShardKeyElement(element))
        return false;

    if (element.type() == BSONType::regEx)
        return false;

    if (element.type() == BSONType::object &&
        !element.embeddedObject().storageValidEmbedded().isOK())
        return false;

    return true;
}

}  // namespace mongo
