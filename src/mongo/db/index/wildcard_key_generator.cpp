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
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/record_id_helpers.h"

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

// Append pre-extracted elements to 'keyString'. This function is used for compound wildcard index
// keys generation.
void appendToKeyString(const std::vector<BSONElement>& elems,
                       const CollatorInterface* collator,
                       KeyString::PooledBuilder* keyString) {
    for (const auto& elem : elems) {
        if (collator) {
            keyString->appendBSONElement(elem, [&](StringData stringData) {
                return collator->getComparisonString(stringData);
            });
        } else {
            keyString->appendBSONElement(elem);
        }
    }
}

// Append 'MinKey' to 'keyString'. Multikey path keys use 'MinKey' for non-wildcard fields.
void appendToMultiKeyString(const std::vector<BSONElement>& elems,
                            KeyString::PooledBuilder* keyString) {
    for (size_t i = 0; i < elems.size(); i++) {
        keyString->appendBSONElement(kMinBSONKey.firstElement());
    }
}

/**
 * A helper class for generating all the various types of keys for a wildcard index.
 *
 * This class stores a bunch of references to avoid copying. Because of this, users should be
 * careful to ensure this object does not outlive any of them.
 */
class SingleDocumentKeyEncoder {
public:
    SingleDocumentKeyEncoder(const KeyString::Version& keyStringVersion,
                             const Ordering& ordering,
                             const CollatorInterface* collator,
                             const boost::optional<RecordId>& id,
                             const boost::optional<KeyFormat>& rsKeyFormat,
                             SharedBufferFragmentBuilder& pooledBufferBuilder,
                             KeyStringSet::sequence_type* keys,
                             KeyStringSet::sequence_type* multikeyPaths,
                             const std::vector<BSONElement>& preElems,
                             const std::vector<BSONElement>& postElems)
        : _keyStringVersion(keyStringVersion),
          _ordering(ordering),
          _collator(collator),
          _id(id),
          _rsKeyFormat(rsKeyFormat),
          _pooledBufferBuilder(pooledBufferBuilder),
          _keys(keys),
          _multikeyPaths(multikeyPaths),
          _preElems(preElems),
          _postElems(postElems) {}

    // Traverses every path of the post-projection document, adding keys to the set as it goes.
    void traverseWildcard(BSONObj obj, bool objIsArray, FieldRef* path);

private:
    // Helper functions to format the entry appropriately before adding it to the key/path tracker.
    void _addMultiKey(const FieldRef& fullPath);

    void _addKey(BSONElement elem, const FieldRef& fullPath);

    // Helper to check whether the element is a nested array, and conditionally add it to 'keys'.
    bool _addKeyForNestedArray(BSONElement elem,
                               const FieldRef& fullPath,
                               bool enclosingObjIsArray);

    bool _addKeyForEmptyLeaf(BSONElement elem, const FieldRef& fullPath);

    const KeyString::Version& _keyStringVersion;
    const Ordering& _ordering;
    const CollatorInterface* _collator;
    const boost::optional<RecordId>& _id;
    const boost::optional<KeyFormat>& _rsKeyFormat;
    SharedBufferFragmentBuilder& _pooledBufferBuilder;
    KeyStringSet::sequence_type* _keys;
    KeyStringSet::sequence_type* _multikeyPaths;
    const std::vector<BSONElement>& _preElems;
    const std::vector<BSONElement>& _postElems;
};

void SingleDocumentKeyEncoder::traverseWildcard(BSONObj obj, bool objIsArray, FieldRef* path) {
    for (const auto& elem : obj) {
        // If the element's fieldName contains a ".", fast-path skip it because it's not queryable.
        if (elem.fieldNameStringData().find('.', 0) != std::string::npos)
            continue;

        // Append the element's fieldname to the path, if the enclosing object is not an array.
        pushPathComponent(elem, objIsArray, path);

        switch (elem.type()) {
            case BSONType::Array:
                // If this is a nested array, we don't descend it but instead index it as a value.
                if (_addKeyForNestedArray(elem, *path, objIsArray)) {
                    break;
                }

                // Add an entry for the multi-key path, and then fall through to BSONType::Object.
                _addMultiKey(*path);
                [[fallthrough]];

            case BSONType::Object:
                if (_addKeyForEmptyLeaf(elem, *path)) {
                    break;
                }

                traverseWildcard(elem.Obj(), elem.type() == BSONType::Array, path);
                break;

            default:
                _addKey(elem, *path);
        }

        // Remove the element's fieldname from the path, if it was pushed onto it earlier.
        popPathComponent(elem, objIsArray, path);
    }
}

void SingleDocumentKeyEncoder::_addKey(BSONElement elem, const FieldRef& fullPath) {
    // Wildcard keys are of the form { "": "path.to.field", "": <collation-aware value> }.
    KeyString::PooledBuilder keyString(_pooledBufferBuilder, _keyStringVersion, _ordering);

    if (!_preElems.empty()) {
        appendToKeyString(_preElems, _collator, &keyString);
    }

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

    if (!_postElems.empty()) {
        appendToKeyString(_postElems, _collator, &keyString);
    }

    if (_id) {
        keyString.appendRecordId(*_id);
    }
    _keys->push_back(keyString.release());
}

void SingleDocumentKeyEncoder::_addMultiKey(const FieldRef& fullPath) {
    // Multikey paths are denoted by a key of the form { "": 1, "": "path.to.array" }. The argument
    // 'multikeyPaths' may be nullptr if the access method is being used in an operation which does
    // not require multikey path generation.
    if (_multikeyPaths) {
        KeyString::PooledBuilder keyString(_pooledBufferBuilder, _keyStringVersion, _ordering);

        if (!_preElems.empty()) {
            appendToMultiKeyString(_preElems, &keyString);
        }
        for (auto elem : BSON("" << 1 << "" << fullPath.dottedField())) {
            keyString.appendBSONElement(elem);
        }
        if (!_postElems.empty()) {
            appendToMultiKeyString(_postElems, &keyString);
        }

        keyString.appendRecordId(record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, *_rsKeyFormat));

        _multikeyPaths->push_back(keyString.release());
    }
}

bool SingleDocumentKeyEncoder::_addKeyForNestedArray(BSONElement elem,
                                                     const FieldRef& fullPath,
                                                     bool enclosingObjIsArray) {
    // If this element is an array whose parent is also an array, index it as a value.
    if (enclosingObjIsArray && elem.type() == BSONType::Array) {
        _addKey(elem, fullPath);
        return true;
    }
    return false;
}

bool SingleDocumentKeyEncoder::_addKeyForEmptyLeaf(BSONElement elem, const FieldRef& fullPath) {
    invariant(elem.isABSONObj());
    if (elem.embeddedObject().isEmpty()) {
        // In keeping with the behaviour of regular indexes, an empty object is indexed as-is while
        // empty arrays are indexed as 'undefined'.
        _addKey(elem.type() == BSONType::Array ? BSONElement{} : elem, fullPath);
        return true;
    }
    return false;
}
}  // namespace

constexpr StringData WildcardKeyGenerator::kSubtreeSuffix;

WildcardProjection WildcardKeyGenerator::createProjectionExecutor(BSONObj keyPattern,
                                                                  BSONObj pathProjection) {
    // TODO SERVER-68303: Remove the invariant after we remove the compound wilcard indexes feature
    // flag.
    // (Ignore FCV check): This is intentional because we want clusters which have wildcard indexes
    // still be able to use the feature even if the FCV is downgraded.
    if (!feature_flags::gFeatureFlagCompoundWildcardIndexes.isEnabledAndIgnoreFCVUnsafe()) {
        // We should never have a key pattern that contains more than a single element.
        invariant(keyPattern.nFields() == 1);
    }

    StringData indexRoot = "";
    size_t suffixPos = std::string::npos;
    for (auto elem : keyPattern) {
        StringData fieldName(elem.fieldNameStringData());
        if (WildcardNames::isWildcardFieldName(fieldName)) {
            // The _keyPattern is either {..., "$**": 1, ..} for all paths or
            // {.., "path.$**": 1, ...} for a single subtree. If we are indexing a single subtree
            // then we will project just that path.
            indexRoot = fieldName;
            suffixPos = indexRoot.find(kSubtreeSuffix);
            break;
        }
    }

    // If we're indexing a single subtree, we can't also specify a path projection.
    uassert(7246102,
            str::stream() << "the wildcard keyPattern " << keyPattern.toString() << " is invalid",
            !indexRoot.empty() && (suffixPos == std::string::npos || pathProjection.isEmpty()));

    auto projSpec = (suffixPos != std::string::npos ? BSON(indexRoot.substr(0, suffixPos) << 1)
                         : pathProjection.isEmpty() ? kDefaultProjection
                                                    : pathProjection);

    // Construct a dummy ExpressionContext for ProjectionExecutor. It's OK to set the
    // ExpressionContext's OperationContext and CollatorInterface to 'nullptr' and the namespace
    // string to '' here; since we ban computed fields from the projection, the ExpressionContext
    // will never be used.
    auto expCtx = make_intrusive<ExpressionContext>(nullptr, nullptr, NamespaceString());
    auto policies = ProjectionPolicies::wildcardIndexSpecProjectionPolicies();
    auto projection = projection_ast::parseAndAnalyze(expCtx, projSpec, policies);
    return WildcardProjection{projection_executor::buildProjectionExecutor(
        expCtx, &projection, policies, projection_executor::kDefaultBuilderParams)};
}

WildcardKeyGenerator::WildcardKeyGenerator(BSONObj keyPattern,
                                           BSONObj pathProjection,
                                           const CollatorInterface* collator,
                                           KeyString::Version keyStringVersion,
                                           Ordering ordering,
                                           boost::optional<KeyFormat> rsKeyFormat)
    : _proj(createProjectionExecutor(keyPattern, pathProjection)),
      _collator(collator),
      _keyPattern(keyPattern),
      _keyStringVersion(keyStringVersion),
      _ordering(ordering),
      _rsKeyFormat(rsKeyFormat) {
    std::vector<const char*> preFields;
    std::vector<const char*> postFields;
    std::vector<BSONElement> preElems;
    std::vector<BSONElement> postElems;
    size_t idx = 0;
    bool iteratorIsBeforeWildcard = true;
    for (auto elem : keyPattern) {
        if (WildcardNames::isWildcardFieldName(elem.fieldNameStringData())) {
            iteratorIsBeforeWildcard = false;
        } else if (iteratorIsBeforeWildcard) {
            preElems.push_back(BSONElement());
            preFields.push_back(elem.fieldName());
        } else {
            postElems.push_back(BSONElement());
            postFields.push_back(elem.fieldName());
        }
        idx++;
    }

    // We should initialize BtreeKeyGenerators if 'keyPattern' is compound.
    if (!preFields.empty()) {
        _preBtreeGenerator.emplace(std::move(preFields),
                                   std::move(preElems),
                                   true /* isSparse */,
                                   _keyStringVersion,
                                   _ordering);
    }
    if (!postFields.empty()) {
        _postBtreeGenerator.emplace(std::move(postFields),
                                    std::move(postElems),
                                    true /* isSparse */,
                                    _keyStringVersion,
                                    _ordering);
    }
}

void WildcardKeyGenerator::generateKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                        BSONObj inputDoc,
                                        KeyStringSet* keys,
                                        KeyStringSet* multikeyPaths,
                                        const boost::optional<RecordId>& id) const {
    tassert(6868511,
            "To add multi keys to 'multikeyPaths', the key format 'rsKeyFormat' must be provided "
            "at the constructor",
            !multikeyPaths || _rsKeyFormat);

    std::vector<BSONElement> preElems;
    std::vector<BSONElement> postElems;
    boost::dynamic_bitset<size_t> preElemsExist;
    boost::dynamic_bitset<size_t> postElemsExist;

    // Extract elements for regular fields if this is a compound wildcard index.
    if (_preBtreeGenerator) {
        preElemsExist = _preBtreeGenerator->extractElements(inputDoc, &preElems);
    }
    if (_postBtreeGenerator) {
        postElemsExist = _postBtreeGenerator->extractElements(inputDoc, &postElems);
    }

    FieldRef rootPath;
    auto keysSequence = keys->extract_sequence();
    auto sequenceSize = keysSequence.size();
    // multikeyPaths is allowed to be nullptr
    KeyStringSet::sequence_type multikeyPathsSequence;
    if (multikeyPaths)
        multikeyPathsSequence = multikeyPaths->extract_sequence();

    SingleDocumentKeyEncoder keyEncoder{_keyStringVersion,
                                        _ordering,
                                        _collator,
                                        id,
                                        _rsKeyFormat,
                                        pooledBufferBuilder,
                                        &keysSequence,
                                        multikeyPaths ? &multikeyPathsSequence : nullptr,
                                        preElems,
                                        postElems};

    keyEncoder.traverseWildcard(
        _proj.exec()->applyTransformation(Document{inputDoc}).toBson(), false, &rootPath);

    // If no key is generated for this index at this point, that means the document doesn't have any
    // field that is indexed by the wildcard field. We should still add index keys for this
    // document if the document has any regular field of a compound wildcad index. For example,
    // a document {a: 1} should still be indexed by this compound wildcard index {a:1, "b.$**": 1}.
    // In this case, we generate an index key {'': 1, '': MinKey, '': MinKey} for this document.
    if (keysSequence.size() == sequenceSize && (!preElems.empty() || !postElems.empty())) {
        KeyString::PooledBuilder keyString(pooledBufferBuilder, _keyStringVersion, _ordering);

        if (preElemsExist.any() || postElemsExist.any()) {
            if (!preElems.empty()) {
                appendToKeyString(preElems, _collator, &keyString);
            }
            // We use 'MinKey' for both the '$_path' field and the wildcard field similar to what we
            // use in multikey-path index keys.
            keyString.appendBSONElement(kMinBSONKey.firstElement());
            keyString.appendBSONElement(kMinBSONKey.firstElement());
            if (!postElems.empty()) {
                appendToKeyString(postElems, _collator, &keyString);
            }
            if (id) {
                keyString.appendRecordId(*id);
            }
            keysSequence.push_back(keyString.release());
        }
    }

    if (multikeyPaths)
        multikeyPaths->adopt_sequence(std::move(multikeyPathsSequence));
    keys->adopt_sequence(std::move(keysSequence));
}
}  // namespace mongo
