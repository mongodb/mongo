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

#include "mongo/db/index/sort_key_generator.h"

#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {

SortKeyGenerator::SortKeyGenerator(SortPattern sortPattern, const CollatorInterface* collator)
    : _collator(collator), _sortPattern(std::move(sortPattern)) {
    BSONObjBuilder btreeBob;
    size_t nFields = 0;

    for (auto&& part : _sortPattern) {
        if (part.fieldPath) {
            btreeBob.append(part.fieldPath->fullPath(), part.isAscending ? 1 : -1);
            ++nFields;
        }
    }

    // The fake index key pattern used to generate Btree keys.
    _sortSpecWithoutMeta = btreeBob.obj();
    _sortHasMeta = nFields < _sortPattern.size();

    // If we're just sorting by meta, don't bother creating an index key generator.
    if (_sortSpecWithoutMeta.isEmpty()) {
        return;
    }

    // We'll need to treat arrays as if we were to create an index over them. that is, we may need
    // to unnest the first level and consider each array element to decide the sort order. In order
    // to do this, we make a BtreeKeyGenerator.
    std::vector<BSONElement> fixed(nFields);
    std::vector<const char*> fieldNames;
    fieldNames.reserve(fixed.size());
    for (auto&& elem : _sortSpecWithoutMeta) {
        fieldNames.push_back(elem.fieldName());
    }

    constexpr bool isSparse = false;
    _indexKeyGen = std::make_unique<BtreeKeyGenerator>(fieldNames,
                                                       fixed,
                                                       isSparse,
                                                       KeyString::Version::kLatestVersion,
                                                       Ordering::make(_sortSpecWithoutMeta));
}

Value SortKeyGenerator::computeSortKey(const WorkingSetMember& wsm) const {
    if (wsm.hasObj()) {
        return computeSortKeyFromDocument(wsm.doc.value(), wsm.metadata());
    }

    return computeSortKeyFromIndexKey(wsm);
}

Value SortKeyGenerator::computeSortKeyFromIndexKey(const WorkingSetMember& member) const {
    invariant(member.getState() == WorkingSetMember::RID_AND_IDX);
    invariant(!_sortHasMeta);

    BSONObjBuilder objBuilder;
    for (auto&& elem : _sortSpecWithoutMeta) {
        BSONElement sortKeyElt;
        invariant(elem.isNumber());
        invariant(member.getFieldDotted(elem.fieldName(), &sortKeyElt));
        // If we were to call 'collationAwareIndexKeyAppend' with a non-simple collation and a
        // 'sortKeyElt' representing a collated index key we would incorrectly encode for the
        // collation twice. This is not currently possible as the query planner will ensure that
        // the plan fetches the data before sort key generation in the case where the index has a
        // non-simple collation.
        CollationIndexKey::collationAwareIndexKeyAppend(sortKeyElt, _collator, &objBuilder);
    }
    return DocumentMetadataFields::deserializeSortKey(isSingleElementKey(), objBuilder.obj());
}

BSONObj SortKeyGenerator::computeSortKeyFromDocument(const BSONObj& obj,
                                                     const DocumentMetadataFields& metadata) const {
    auto sortKeyNoMetadata = uassertStatusOK(computeSortKeyFromDocumentWithoutMetadata(obj));

    if (!_sortHasMeta) {
        // We don't have to worry about $meta sort, so the index key becomes the sort key.
        return sortKeyNoMetadata;
    }

    BSONObjBuilder mergedKeyBob;

    // Merge metadata into the key.
    BSONObjIterator sortKeyIt(sortKeyNoMetadata);
    for (auto& part : _sortPattern) {
        if (part.fieldPath) {
            invariant(sortKeyIt.more());
            mergedKeyBob.append(sortKeyIt.next());
            continue;
        }

        // Create a Document that represents the input object and its metadata together, so we can
        // use it to evaluate the ExpressionMeta for this part of the sort pattern. This operation
        // copies the data in 'metadata' but not any of the data in the 'obj' BSON.
        MutableDocument documentWithMetdata(Document{obj});
        documentWithMetdata.setMetadata(DocumentMetadataFields(metadata));

        invariant(part.expression);
        auto value =
            part.expression->evaluate(documentWithMetdata.freeze(), nullptr /* variables */);
        if (!value.missing()) {
            value.addToBsonObj(&mergedKeyBob, ""_sd);
        } else {
            mergedKeyBob.appendNull("");
        }
    }

    // We should have added a key component for each part of the index key pattern.
    invariant(!sortKeyIt.more());

    return mergedKeyBob.obj();
}

StatusWith<BSONObj> SortKeyGenerator::computeSortKeyFromDocumentWithoutMetadata(
    const BSONObj& obj) const {
    // Not sorting by anything in the key, just bail out early.
    if (_sortSpecWithoutMeta.isEmpty()) {
        return BSONObj();
    }

    // We will sort 'obj' in the same order an index over '_sortSpecWithoutMeta' would have. This is
    // tricky. Consider the sort pattern {a:1} and the document {a: [1, 10]}. We have potentially
    // two keys we could use to sort on. Here we extract these keys.
    //
    // The keys themselves will incorporate the collation, with strings translated to their
    // corresponding collation keys. Therefore, we use the simple string comparator when comparing
    // the keys themselves.
    KeyStringSet keys;
    SharedBufferFragmentBuilder allocator(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

    try {
        // There's no need to compute the prefixes of the indexed fields that cause the index to be
        // multikey when getting the index keys for sorting.
        MultikeyPaths* multikeyPaths = nullptr;
        const auto skipMultikey = false;
        _indexKeyGen->getKeys(allocator, obj, skipMultikey, &keys, multikeyPaths, _collator);
    } catch (const AssertionException& e) {
        // Probably a parallel array.
        if (ErrorCodes::CannotIndexParallelArrays == e.code()) {
            return Status(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");
        } else {
            return e.toStatus();
        }
    } catch (...) {
        return Status(ErrorCodes::InternalError, "unknown error during sort key generation");
    }

    // Key generator isn't sparse so we should at least get an all-null key.
    invariant(!keys.empty());

    // The sort key is the first index key, ordered according to the pattern '_sortSpecWithoutMeta'.
    return KeyString::toBson(*keys.begin(), Ordering::make(_sortSpecWithoutMeta));
}

Value SortKeyGenerator::getCollationComparisonKey(const Value& val) const {
    // If the collation is the simple collation, the value itself is the comparison key.
    if (!_collator) {
        return val;
    }

    // If 'val' is not a collatable type, there's no need to do any work.
    if (!CollationIndexKey::isCollatableType(val.getType())) {
        return val;
    }

    // If 'val' is a string, directly use the collator to obtain a comparison key.
    if (val.getType() == BSONType::String) {
        auto compKey = _collator->getComparisonKey(val.getString());
        return Value(compKey.getKeyData());
    }

    // Otherwise, for non-string collatable types, take the slow path and round-trip the value
    // through BSON.
    BSONObjBuilder input;
    val.addToBsonObj(&input, ""_sd);

    BSONObjBuilder output;
    CollationIndexKey::collationAwareIndexKeyAppend(input.obj().firstElement(), _collator, &output);
    return Value(output.obj().firstElement());
}

boost::optional<Value> SortKeyGenerator::extractKeyPart(
    const Document& doc,
    const DocumentMetadataFields& metadata,
    const SortPattern::SortPatternPart& patternPart) const {
    Value plainKey;
    if (patternPart.fieldPath) {
        invariant(!patternPart.expression);
        auto keyVariant = doc.getNestedFieldNonCaching(*patternPart.fieldPath);

        auto key = stdx::visit(
            OverloadedVisitor{
                // In this case, the document has an array along the path given. This means the
                // document is ineligible for taking the fast path for index key generation.
                [](Document::TraversesArrayTag) -> boost::optional<Value> { return boost::none; },
                // In this case the field was already in the cache (or may not have existed).
                [](const Value& val) -> boost::optional<Value> {
                    // The document may have an array at the given path.
                    if (val.getType() == BSONType::Array) {
                        return boost::none;
                    }
                    return val;
                },
                // In this case the field was in the backing BSON, and not in the cache.
                [](BSONElement elt) -> boost::optional<Value> {
                    // The document may have an array at the given path.
                    if (elt.type() == BSONType::Array) {
                        return boost::none;
                    }
                    return Value(elt);
                },
                [](stdx::monostate none) -> boost::optional<Value> { return Value(); },
            },
            keyVariant);

        if (!key) {
            return boost::none;
        }
        plainKey = std::move(*key);
    } else {
        invariant(patternPart.expression);
        // ExpressionMeta expects metadata to be attached to the document.
        MutableDocument documentWithMetadata(doc);
        documentWithMetadata.setMetadata(DocumentMetadataFields(metadata));

        // ExpressionMeta does not use Variables.
        plainKey = patternPart.expression->evaluate(documentWithMetadata.freeze(),
                                                    nullptr /* variables */);
    }

    return plainKey.missing() ? Value{BSONNULL} : getCollationComparisonKey(plainKey);
}

boost::optional<Value> SortKeyGenerator::extractKeyFast(
    const Document& doc, const DocumentMetadataFields& metadata) const {
    if (_sortPattern.isSingleElementKey()) {
        return extractKeyPart(doc, metadata, _sortPattern[0]);
    }

    std::vector<Value> keys;
    keys.reserve(_sortPattern.size());
    for (auto&& keyPart : _sortPattern) {
        auto extractedKey = extractKeyPart(doc, metadata, keyPart);
        if (!extractedKey) {
            // We can't use the fast path, so bail out.
            return extractedKey;
        }

        keys.push_back(std::move(*extractedKey));
    }
    return Value{std::move(keys)};
}

BSONObj SortKeyGenerator::extractKeyWithArray(const Document& doc,
                                              const DocumentMetadataFields& metadata) const {
    // Sort key generation requires the Document to be in BSON format. First, we attempt the
    // "trivial" conversion, which returns the Document's BSON storage.
    auto optionalBsonDoc = doc.toBsonIfTriviallyConvertible();

    // If the trivial conversion is not possible, we perform a conversion that only converts the
    // paths we need to generate the sort key.
    auto bsonDoc = optionalBsonDoc ? std::move(*optionalBsonDoc)
                                   : _sortPattern.documentToBsonWithSortPaths(doc);

    return computeSortKeyFromDocument(bsonDoc, metadata);
}

Value SortKeyGenerator::computeSortKeyFromDocument(const Document& doc,
                                                   const DocumentMetadataFields& metadata) const {
    // This fast pass directly generates a Value.
    auto fastKey = extractKeyFast(doc, metadata);
    if (fastKey) {
        return std::move(*fastKey);
    }

    // Compute the key through the slow path, which generates a serialized BSON sort key (taking a
    // form like BSONObj {'': 1, '': [2, 3]}) and converts it to a Value (Value [1, [2, 3]] in the
    // earlier example).
    return DocumentMetadataFields::deserializeSortKey(_sortPattern.isSingleElementKey(),
                                                      extractKeyWithArray(doc, metadata));
}

}  // namespace mongo
