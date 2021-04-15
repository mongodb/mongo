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

#include "mongo/db/index/wildcard_key_generator.h"

#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/projection_parser.h"

namespace mongo {
namespace {

// If the user does not specify any projection, then we default to a projection of {_id: 0} in order
// to prevent the _id field from being indexed, since it already has its own dedicated index.
static const BSONObj kDefaultProjection = BSON("_id"_sd << 0);

// If the enclosing object is an array, then the current element's fieldname is the array index, so
// we omit this when computing the full path. Otherwise, the full path is the pathPrefix plus the
// element's fieldname.
void pushPathComponent(BSONElement elem, bool enclosingObjIsArray, FieldRef* pathPrefix) {
    if (!enclosingObjIsArray) {
        pathPrefix->appendPart(elem.fieldNameStringData());
    }
}

// If the enclosing object is not an array, then the final path component should be its field name.
// Verify that this is the case and then pop it off the FieldRef.
void popPathComponent(BSONElement elem, bool enclosingObjIsArray, FieldRef* pathToElem) {
    if (!enclosingObjIsArray) {
        invariant(pathToElem->getPart(pathToElem->numParts() - 1) == elem.fieldNameStringData());
        pathToElem->removeLastPart();
    }
}

void validateWildcardIndexKeys(const std::vector<StringData>& nonWildcardFields,
                               const StringData& wildcardField,
                               const std::vector<StringData>& projectionInclusionSet,
                               const std::set<StringData>& projectionExclusionSet) {
    for (const auto& nonWildcardField : nonWildcardFields) {
        if (wildcardField == "$**") {
            if (projectionInclusionSet.size()) {
                for (const auto& projInc : projectionInclusionSet) {
                    uassert(
                        9999901,
                        "Compound wildcard index fields overlaps with the projected wildcard field",
                        !(projInc == nonWildcardField ||
                          expression::isPathPrefixOf(projInc, nonWildcardField) ||
                          expression::isPathPrefixOf(nonWildcardField, projInc)));
                }
            } else {
                uassert(9999902,
                        "Wildcard index does not allow compound key fields with the toplevel "
                        "wildcard field without exclusion projection on them",
                        std::find_if(projectionExclusionSet.begin(),
                                     projectionExclusionSet.end(),
                                     [&](const auto& exclude) {
                                         return exclude == nonWildcardField ||
                                             expression::isPathPrefixOf(exclude, nonWildcardField);
                                     }) != projectionExclusionSet.end());
            }
        }
        uassert(9999903,
                "Compound wildcard index does not allow fields overlapping with the wildcard field",
                !(wildcardField == nonWildcardField ||
                  expression::isPathPrefixOf(wildcardField, nonWildcardField) ||
                  expression::isPathPrefixOf(nonWildcardField, wildcardField)));
    }
}
}  // namespace

constexpr StringData WildcardKeyGenerator::kSubtreeSuffix;

WildcardProjection WildcardKeyGenerator::createProjectionExecutor(BSONObj keyPattern,
                                                                  BSONObj pathProjection) {
    WildcardProjection* proj = nullptr;
    std::vector<StringData> nonWildcardFields;
    StringData wildcardField;
    for (const auto& elem : keyPattern) {
        if (auto keyStr = elem.fieldNameStringData();
            (keyStr == "$**") || keyStr.endsWith(".$**")) {
            uassert(
                9999900, "Wildcard index does not allow multiple wildcard compound keys", !proj);
            // The _keyPattern is either { "$**": 1 } for all paths or { "path.$**": 1 } for a
            // single subtree. If we are indexing a single subtree, then we will project just that
            // path.
            auto suffixPos = keyStr.find(kSubtreeSuffix);
            wildcardField = keyStr.substr(0, suffixPos);

            // If we're indexing a single subtree, we can't also specify a path projection.
            invariant(suffixPos == std::string::npos || pathProjection.isEmpty());

            // If this is a subtree projection, the projection spec is { "path.to.subtree": 1 }.
            // Otherwise, we use the path projection from the original command object. If the path
            // projection is empty we default to {_id: 0}, since empty projections are illegal and
            // will be rejected when parsed.
            auto projSpec = (suffixPos != std::string::npos
                                 ? BSON(wildcardField << 1)
                                 : pathProjection.isEmpty() ? kDefaultProjection : pathProjection);

            // Construct a dummy ExpressionContext for ProjectionExecutor. It's OK to set the
            // ExpressionContext's OperationContext and CollatorInterface to 'nullptr' and the
            // namespace string to '' here; since we ban computed fields from the projection, the
            // ExpressionContext will never be used.
            auto expCtx = make_intrusive<ExpressionContext>(nullptr, nullptr, NamespaceString());
            auto policies = ProjectionPolicies::wildcardIndexSpecProjectionPolicies();
            auto projection = projection_ast::parse(expCtx, projSpec, policies);
            proj = new WildcardProjection{projection_executor::buildProjectionExecutor(
                expCtx, &projection, policies, projection_executor::kDefaultBuilderParams)};
        } else {
            nonWildcardFields.push_back(elem.fieldNameStringData());
        }
    }
    // The projections on wildcard are only used when wildcardField is "$**" and are mutually
    // exclusive.
    std::vector<StringData> projectionInclusionSet;
    std::set<StringData> projectionExclusionSet;
    for (const auto& proj : pathProjection) {
        if (proj.numberInt() == 1) {
            projectionInclusionSet.push_back(proj.fieldNameStringData());
        } else {
            projectionExclusionSet.insert(proj.fieldNameStringData());
        }
    }
    validateWildcardIndexKeys(
        nonWildcardFields, wildcardField, projectionInclusionSet, projectionExclusionSet);
    return std::move(*proj);
}

WildcardKeyGenerator::WildcardKeyGenerator(BSONObj keyPattern,
                                           BSONObj pathProjection,
                                           const CollatorInterface* collator,
                                           KeyString::Version keyStringVersion,
                                           Ordering ordering)
    : _proj(createProjectionExecutor(keyPattern, pathProjection)),
      _collator(collator),
      _keyPattern(keyPattern),
      _keyStringVersion(keyStringVersion),
      _ordering(ordering) {
    std::vector<const char*> fieldNames;
    for (const auto& elem : _keyPattern) {
        if (auto keyStr = elem.fieldNameStringData();
            (keyStr != "$**") && !keyStr.endsWith(".$**")) {
            fieldNames.push_back(elem.fieldName());
        }
    }
    std::vector<BSONElement> fixed(fieldNames.size());
    _indexKeyGen = std::make_unique<BtreeKeyGenerator>(
        fieldNames, fixed, true /* isSparse */, _collator, _keyStringVersion, _ordering);
}

void WildcardKeyGenerator::generateKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                        BSONObj inputDoc,
                                        KeyStringSet* keys,
                                        KeyStringSet* multikeyPaths,
                                        boost::optional<RecordId> id) const {
    KeyStringSet nonWildcardKeys;
    SharedBufferFragmentBuilder allocator(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);
    MultikeyPaths nonWildcardMultikeyPaths;
    const auto skipMultikey = false;
    _indexKeyGen->getKeys(
        allocator, inputDoc, skipMultikey, &nonWildcardKeys, &nonWildcardMultikeyPaths);

    // multikeyPaths is allowed to be nullptr
    KeyStringSet::sequence_type multikeyPathsSequence;
    if (multikeyPaths)
        multikeyPathsSequence = multikeyPaths->extract_sequence();
    BSONObjIterator keyPatternItr(_keyPattern);
    for (const auto& component : nonWildcardMultikeyPaths) {
        auto keyStr = (*keyPatternItr).fieldNameStringData();
        if ((keyStr != "$**") && !keyStr.endsWith(".$**")) {
            for (const auto& depth : component) {
                _addMultiKey(pooledBufferBuilder,
                             FieldRef(FieldRef(keyStr).dottedSubstring(0, depth + 1)),
                             &multikeyPathsSequence);
            }
        }
        ++keyPatternItr;
    }

    auto projected = _proj.exec()->applyTransformation(Document{inputDoc}).toBson();
    if (projected.isEmpty()) {
        *keys = std::move(nonWildcardKeys);
        return;
    }
    FieldRef rootPath;
    auto keysSequence = keys->extract_sequence();
    _traverseWildcard(pooledBufferBuilder,
                      projected,
                      false,
                      &rootPath,
                      &nonWildcardKeys,
                      &keysSequence,
                      multikeyPaths ? &multikeyPathsSequence : nullptr,
                      id);
    if (multikeyPaths)
        multikeyPaths->adopt_sequence(std::move(multikeyPathsSequence));
    keys->adopt_sequence(std::move(keysSequence));
}

void WildcardKeyGenerator::_traverseWildcard(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                             BSONObj obj,
                                             bool objIsArray,
                                             FieldRef* path,
                                             KeyStringSet* nonWildcardKeys,
                                             KeyStringSet::sequence_type* keys,
                                             KeyStringSet::sequence_type* multikeyPaths,
                                             boost::optional<RecordId> id) const {
    for (const auto& elem : obj) {
        // If the element's fieldName contains a ".", fast-path skip it because it's not queryable.
        if (elem.fieldNameStringData().find('.', 0) != std::string::npos)
            continue;

        // Append the element's fieldname to the path, if the enclosing object is not an array.
        pushPathComponent(elem, objIsArray, path);

        switch (elem.type()) {
            case BSONType::Array:
                // If this is a nested array, we don't descend it but instead index it as a value.
                if (_addKeyForNestedArray(
                        pooledBufferBuilder, elem, *path, objIsArray, nonWildcardKeys, keys, id))
                    break;

                // Add an entry for the multi-key path, and then fall through to BSONType::Object.
                _addMultiKey(pooledBufferBuilder, *path, multikeyPaths);

            case BSONType::Object:
                if (_addKeyForEmptyLeaf(
                        pooledBufferBuilder, elem, *path, nonWildcardKeys, keys, id))
                    break;

                _traverseWildcard(pooledBufferBuilder,
                                  elem.Obj(),
                                  elem.type() == BSONType::Array,
                                  path,
                                  nonWildcardKeys,
                                  keys,
                                  multikeyPaths,
                                  id);
                break;

            default:
                _addKey(pooledBufferBuilder, elem, *path, nonWildcardKeys, keys, id);
        }

        // Remove the element's fieldname from the path, if it was pushed onto it earlier.
        popPathComponent(elem, objIsArray, path);
    }
}

bool WildcardKeyGenerator::_addKeyForNestedArray(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                                 BSONElement elem,
                                                 const FieldRef& fullPath,
                                                 bool enclosingObjIsArray,
                                                 KeyStringSet* nonWildcardKeys,
                                                 KeyStringSet::sequence_type* keys,
                                                 boost::optional<RecordId> id) const {
    // If this element is an array whose parent is also an array, index it as a value.
    if (enclosingObjIsArray && elem.type() == BSONType::Array) {
        _addKey(pooledBufferBuilder, elem, fullPath, nonWildcardKeys, keys, id);
        return true;
    }
    return false;
}

bool WildcardKeyGenerator::_addKeyForEmptyLeaf(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                               BSONElement elem,
                                               const FieldRef& fullPath,
                                               KeyStringSet* nonWildcardKeys,
                                               KeyStringSet::sequence_type* keys,
                                               boost::optional<RecordId> id) const {
    invariant(elem.isABSONObj());
    if (elem.embeddedObject().isEmpty()) {
        // In keeping with the behaviour of regular indexes, an empty object is indexed as-is while
        // empty arrays are indexed as 'undefined'.
        _addKey(pooledBufferBuilder,
                elem.type() == BSONType::Array ? BSONElement{} : elem,
                fullPath,
                nonWildcardKeys,
                keys,
                id);
        return true;
    }
    return false;
}

void WildcardKeyGenerator::_addWildcard(KeyString::PooledBuilder& keyString,
                                        BSONElement elem,
                                        const FieldRef& fullPath,
                                        KeyStringSet::sequence_type* keys,
                                        boost::optional<RecordId> id) const {
    keyString.appendString(fullPath.dottedField());
    if (_collator && elem) {
        keyString.appendBSONElement(elem, [&](StringData stringData) {
            return _collator->getComparisonString(stringData);
        });
    } else if (elem) {
        keyString.appendBSONElement(elem);
    } else {
        keyString.appendUndefined();
    }
}

void WildcardKeyGenerator::_addKey(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                   BSONElement elem,
                                   const FieldRef& fullPath,
                                   KeyStringSet* nonWildcardKeys,
                                   KeyStringSet::sequence_type* keys,
                                   boost::optional<RecordId> id) const {
    if (nonWildcardKeys->size()) {
        for (auto nonWildcardKeysIter = nonWildcardKeys->begin();
             nonWildcardKeysIter != nonWildcardKeys->end();
             ++nonWildcardKeysIter) {
            // Wildcard keys are of the form { "": "path.to.field", "": <collation-aware value> }.
            KeyString::PooledBuilder keyString(pooledBufferBuilder, _keyStringVersion, _ordering);
            auto decodedNonWildcardKey = KeyString::toBson(*nonWildcardKeysIter, _ordering);
            BSONObjIterator decodeKeysIter(decodedNonWildcardKey);
            for (auto&& keyPatternElem : _keyPattern) {
                if (auto keyStr = keyPatternElem.fieldNameStringData();
                    (keyStr == "$**") || keyStr.endsWith(".$**")) {
                    _addWildcard(keyString, elem, fullPath, keys, id);
                } else {
                    keyString.appendBSONElement(decodeKeysIter.next());
                }
            }
            if (id) {
                keyString.appendRecordId(*id);
            }
            keys->push_back(keyString.release());
        }
    } else {
        KeyString::PooledBuilder keyString(pooledBufferBuilder, _keyStringVersion, _ordering);
        _addWildcard(keyString, elem, fullPath, keys, id);
        if (id) {
            keyString.appendRecordId(*id);
        }
        keys->push_back(keyString.release());
    }
}

void WildcardKeyGenerator::_addMultiKey(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                        const FieldRef& fullPath,
                                        KeyStringSet::sequence_type* multikeyPaths) const {
    // Multikey paths are denoted by a key of the form { "": 1, "": "path.to.array" }. The argument
    // 'multikeyPaths' may be nullptr if the access method is being used in an operation which does
    // not require multikey path generation.
    if (multikeyPaths) {
        BSONObjBuilder keyBuilder;
        for (auto&& elem : _keyPattern) {
            if (elem.fieldNameStringData() == "$**" ||
                elem.fieldNameStringData().endsWith(".$**")) {
                keyBuilder.append("", 1);
                keyBuilder.append("", fullPath.dottedField());
            } else {
                keyBuilder.appendMinKey("");
            }
        }
        KeyString::PooledBuilder keyString(
            pooledBufferBuilder,
            _keyStringVersion,
            keyBuilder.obj(),
            _ordering,
            RecordIdReservations::reservedIdFor(ReservationId::kWildcardMultikeyMetadataId));
        multikeyPaths->push_back(keyString.release());
    }
}

}  // namespace mongo
