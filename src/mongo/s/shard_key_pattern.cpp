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

#include "mongo/s/shard_key_pattern.h"

#include <vector>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/path_internal.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/update/path_support.h"
#include "mongo/util/str.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {

using pathsupport::EqualityMatches;

namespace {

// Maximum number of intervals produced by $in queries
constexpr size_t kMaxFlattenedInCombinations = 4000000;

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
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Field " << patternEl.fieldNameStringData()
                                  << " contains empty parts",
                    !newFieldRef->getPart(i).empty());
        }

        // Numeric and ascending (1.0), or "hashed" with exactly hashed field.
        auto isHashedPattern = ShardKeyPattern::isHashedPatternEl(patternEl);
        numHashedFields += isHashedPattern ? 1 : 0;
        uassert(ErrorCodes::BadValue,
                str::stream() << "Shard key " << keyPattern.toString()
                              << " can contain at most one 'hashed' field, and/or multiple "
                                 "numerical fields set to a value of 1. Failed to parse field "
                              << patternEl.fieldNameStringData(),
                (patternEl.isNumber() && patternEl.safeNumberInt() == 1) ||
                    (isHashedPattern && numHashedFields == 1));
        parsedPaths.emplace_back(std::move(newFieldRef));
    }

    return parsedPaths;
}

bool isValidShardKeyElementForExtractionFromDocument(const BSONElement& element) {
    return element.type() != Array;
}

bool isValidShardKeyElement(const BSONElement& element) {
    return !element.eoo() && element.type() != Array;
}

bool isValidShardKeyElementForStorage(const BSONElement& element) {
    if (!isValidShardKeyElement(element))
        return false;

    if (element.type() == RegEx)
        return false;

    if (element.type() == Object && !element.embeddedObject().storageValidEmbedded().isOK())
        return false;

    return true;
}

BSONElement extractKeyElementFromDoc(const BSONObj& obj, StringData pathStr) {
    // Any arrays found get immediately returned. We are equipped up the call stack to specifically
    // deal with array values.
    size_t idxPath;
    return getFieldDottedOrArray(obj, FieldRef(pathStr), &idxPath);
}

BSONElement findEqualityElement(const EqualityMatches& equalities, const FieldRef& path) {
    int parentPathPart;
    const BSONElement parentEl =
        pathsupport::findParentEqualityElement(equalities, path, &parentPathPart);

    if (parentPathPart == static_cast<int>(path.numParts()))
        return parentEl;

    if (parentEl.type() != Object)
        return BSONElement();

    StringData suffixStr = path.dottedSubstring(parentPathPart, path.numParts());
    return extractKeyElementFromDoc(parentEl.Obj(), suffixStr);
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
    return el.type() == String && el.String() == IndexNames::HASHED;
}

bool ShardKeyPattern::isHashedPattern() const {
    return !_hashedField.eoo();
}

bool ShardKeyPattern::isValidHashedValue(const BSONElement& el) {
    switch (el.type()) {
        case MinKey:
        case MaxKey:
        case NumberLong:
            return true;
        default:
            return false;
    }
    MONGO_UNREACHABLE;
}


bool ShardKeyPattern::hasHashedPrefix() const {
    return isHashedPatternEl(_keyPattern.toBSON().firstElement());
}

BSONElement ShardKeyPattern::getHashedField() const {
    return _hashedField;
}

const KeyPattern& ShardKeyPattern::getKeyPattern() const {
    return _keyPattern;
}

const std::vector<std::unique_ptr<FieldRef>>& ShardKeyPattern::getKeyPatternFields() const {
    return _keyPatternPaths;
}

const BSONObj& ShardKeyPattern::toBSON() const {
    return _keyPattern.toBSON();
}

std::string ShardKeyPattern::toString() const {
    return toBSON().toString();
}

std::string ShardKeyPattern::toKeyString(const BSONObj& shardKey) {
    KeyString::Builder ks(KeyString::Version::V1, Ordering::allAscending());

    BSONObjIterator it(shardKey);
    while (auto elem = it.next()) {
        ks.appendBSONElement(elem);
    }

    return {ks.getBuffer(), ks.getSize()};
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

BSONObj ShardKeyPattern::extractShardKeyFromOplogEntry(const repl::OplogEntry& entry) const {
    if (!entry.isCrudOpType()) {
        return BSONObj();
    }

    auto objWithDocumentKey = entry.getObjectContainingDocumentKey();

    if (!entry.isUpdateOrDelete()) {
        return extractShardKeyFromDoc(objWithDocumentKey);
    }

    return extractShardKeyFromDocumentKey(objWithDocumentKey);
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

StatusWith<BSONObj> ShardKeyPattern::extractShardKeyFromQuery(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const BSONObj& basicQuery) const {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(basicQuery.getOwned());

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return extractShardKeyFromQuery(*statusWithCQ.getValue());
}

StatusWith<BSONObj> ShardKeyPattern::extractShardKeyFromQuery(
    boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& basicQuery) const {
    auto findCommand = std::make_unique<FindCommandRequest>(expCtx->ns);
    findCommand->setFilter(basicQuery.getOwned());
    if (!expCtx->getCollatorBSON().isEmpty()) {
        findCommand->setCollation(expCtx->getCollatorBSON().getOwned());
    }

    auto statusWithCQ =
        CanonicalQuery::canonicalize(expCtx->opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return extractShardKeyFromQuery(*statusWithCQ.getValue());
}

BSONObj ShardKeyPattern::extractShardKeyFromQuery(const CanonicalQuery& query) const {
    // Extract equalities from query.
    EqualityMatches equalities;
    // TODO: Build the path set initially?
    FieldRefSet keyPatternPathSet(transitional_tools_do_not_use::unspool_vector(_keyPatternPaths));
    // We only care about extracting the full key pattern paths - if they don't exist (or are
    // conflicting), we don't contain the shard key.
    Status eqStatus =
        pathsupport::extractFullEqualityMatches(*query.root(), keyPatternPathSet, &equalities);
    // NOTE: Failure to extract equality matches just means we return no shard key - it's not
    // an error we propagate
    if (!eqStatus.isOK())
        return BSONObj();

    // Extract key from equalities
    // NOTE: The method below is equivalent to constructing a BSONObj and running
    // extractShardKeyFromDoc, but doesn't require creating the doc.

    BSONObjBuilder keyBuilder;
    // Iterate the parsed paths to avoid re-parsing
    for (auto it = _keyPatternPaths.begin(); it != _keyPatternPaths.end(); ++it) {
        const FieldRef& patternPath = **it;
        BSONElement equalEl = findEqualityElement(equalities, patternPath);

        if (!isValidShardKeyElementForStorage(equalEl))
            return BSONObj();

        if (_hashedField && _hashedField.fieldNameStringData() == patternPath.dottedField()) {
            keyBuilder.append(
                patternPath.dottedField(),
                BSONElementHasher::hash64(equalEl, BSONElementHasher::DEFAULT_HASH_SEED));
        } else {
            // NOTE: The equal element may *not* have the same field name as the path - nested $and,
            // $eq, for example
            keyBuilder.appendAs(equalEl, patternPath.dottedField());
        }
    }

    dassert(isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

bool ShardKeyPattern::isUniqueIndexCompatible(const BSONObj& uniqueIndexPattern) const {
    if (!uniqueIndexPattern.isEmpty() && uniqueIndexPattern.firstElementFieldName() == kIdField) {
        return true;
    }

    return _keyPattern.toBSON().isFieldNamePrefixOf(uniqueIndexPattern);
}

BoundList ShardKeyPattern::flattenBounds(const IndexBounds& indexBounds) const {
    invariant(indexBounds.fields.size() == (size_t)_keyPattern.toBSON().nFields());

    // If any field is unsatisfied, return empty bound list.
    for (const auto& field : indexBounds.fields) {
        if (field.intervals.empty()) {
            return BoundList();
        }
    }

    // To construct our bounds we will generate intervals based on bounds for the first field, then
    // compound intervals based on constraints for the first 2 fields, then compound intervals for
    // the first 3 fields, etc.
    //
    // As we loop through the fields, we start generating new intervals that will later get extended
    // in another iteration of the loop. We define these partially constructed intervals using pairs
    // of BSONObjBuilders (shared_ptrs, since after one iteration of the loop they still must exist
    // outside their scope).
    using BoundBuilders = std::vector<std::pair<BSONObjBuilder, BSONObjBuilder>>;

    BoundBuilders builders;
    builders.emplace_back();

    BSONObjIterator keyIter(_keyPattern.toBSON());
    // Until equalityOnly is false, we are just dealing with equality (no range or $in queries).
    bool equalityOnly = true;

    for (size_t i = 0; i < indexBounds.fields.size(); ++i) {
        BSONElement e = keyIter.next();

        StringData fieldName = e.fieldNameStringData();

        // Get the relevant intervals for this field, but we may have to transform the list of
        // what's relevant according to the expression for this field
        const OrderedIntervalList& oil = indexBounds.fields[i];
        const auto& intervals = oil.intervals;

        if (equalityOnly) {
            if (intervals.size() == 1 && intervals.front().isPoint()) {
                // This field is only a single point-interval
                for (auto& builder : builders) {
                    builder.first.appendAs(intervals.front().start, fieldName);
                    builder.second.appendAs(intervals.front().end, fieldName);
                }
            } else {
                // This clause is the first to generate more than a single point. We only execute
                // this clause once. After that, we simplify the bound extensions to prevent
                // combinatorial explosion.
                equalityOnly = false;

                BoundBuilders newBuilders;

                for (auto& builder : builders) {
                    BSONObj first = builder.first.obj();
                    BSONObj second = builder.second.obj();

                    for (const auto& interval : intervals) {
                        uassert(17439,
                                "combinatorial limit of $in partitioning of results exceeded",
                                newBuilders.size() < kMaxFlattenedInCombinations);

                        newBuilders.emplace_back();

                        newBuilders.back().first.appendElements(first);
                        newBuilders.back().first.appendAs(interval.start, fieldName);

                        newBuilders.back().second.appendElements(second);
                        newBuilders.back().second.appendAs(interval.end, fieldName);
                    }
                }

                builders = std::move(newBuilders);
            }
        } else {
            // If we've already generated a range or multiple point-intervals just extend what we've
            // generated with min/max bounds for this field
            for (auto& builder : builders) {
                builder.first.appendAs(intervals.front().start, fieldName);
                builder.second.appendAs(intervals.back().end, fieldName);
            }
        }
    }

    BoundList ret;
    for (auto& builder : builders) {
        ret.emplace_back(builder.first.obj(), builder.second.obj());
    }

    return ret;
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

}  // namespace mongo
