// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/shard_role/shard_catalog/index_catalog.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/assert_util.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {
using namespace std::literals::string_view_literals;
using IndexIterator = IndexCatalog::IndexIterator;

bool IndexIterator::more() {
    if (_start) {
        _next = _advance();
        _start = false;
    }
    return _next != nullptr;
}

const IndexCatalogEntry* IndexIterator::next() {
    if (!more())
        return nullptr;
    _prev = _next;
    _next = _advance();
    return _prev;
}

// Returns normalized version of 'indexSpec' for the catalog.
BSONObj IndexCatalog::normalizeIndexSpec(OperationContext* opCtx,
                                         const CollectionPtr& collection,
                                         const BSONObj& indexSpec) {
    // Add collection-default collation where needed and normalize the collation in each index spec.
    auto normalSpec =
        uassertStatusOK(collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpec));

    // We choose not to normalize the spec's partialFilterExpression at this point, if it exists.
    // Doing so often reduces the legibility of the filter to the end-user, and makes it difficult
    // for clients to validate (via the listIndexes output) whether a given partialFilterExpression
    // is equivalent to the filter that they originally submitted. Omitting this normalization does
    // not impact our internal index comparison semantics, since we compare based on the parsed
    // MatchExpression trees rather than the serialized BSON specs.
    //
    // For similar reasons we do not normalize index projection objects here, if any, so their
    // original forms get persisted in the catalog. Projection normalization to detect whether a
    // candidate new index would duplicate an existing index is done only in the memory-only
    // 'IndexDescriptor._normalizedProjection' field.

    return normalSpec;
}

std::vector<BSONObj> IndexCatalog::normalizeIndexSpecs(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const std::vector<BSONObj>& indexSpecs) {
    std::vector<BSONObj> results;
    results.reserve(indexSpecs.size());

    for (const auto& originalSpec : indexSpecs) {
        results.emplace_back(normalizeIndexSpec(opCtx, collection, originalSpec));
    }

    return results;
}

std::vector<BSONObj> IndexCatalog::normalizeIndexSpecsFromListIndexes(
    const std::vector<BSONObj>& indexSpecs) {
    std::vector<BSONObj> results;
    results.reserve(indexSpecs.size());

    for (const auto& indexSpec : indexSpecs) {
        results.emplace_back(normalizeIndexSpecFromListIndexes(indexSpec));
    }
    return results;
}

BSONObj IndexCatalog::normalizeIndexSpecFromListIndexes(const BSONObj& indexSpec) {

    auto removeSimpleCollationFromBsonObj = [](BSONObj obj) -> BSONObj {
        if (obj.hasField(IndexDescriptor::kCollationFieldName) &&
            SimpleBSONObjComparator::kInstance.evaluate(
                obj[IndexDescriptor::kCollationFieldName].Obj() == CollationSpec::kSimpleSpec)) {
            return obj.removeField(IndexDescriptor::kCollationFieldName);
        }
        return obj;
    };

    BSONObjBuilder bob;

    // Remove simple collation under 'spec' field
    constexpr auto kSpecFieldName = "spec"sv;
    if (indexSpec.hasField(kSpecFieldName)) {
        bob.append(kSpecFieldName,
                   removeSimpleCollationFromBsonObj(indexSpec[kSpecFieldName].Obj()));
    }

    // Remove simple collation under 'originalSpec' field
    if (indexSpec.hasField(IndexDescriptor::kOriginalSpecFieldName)) {
        bob.append(IndexDescriptor::kOriginalSpecFieldName,
                   removeSimpleCollationFromBsonObj(
                       indexSpec[IndexDescriptor::kOriginalSpecFieldName].Obj()));
    }

    // Remove top-level simple collation if exists
    bob.appendElementsUnique(removeSimpleCollationFromBsonObj(indexSpec));

    return bob.obj();
}

Status IndexCatalog::canCreateIndex(OperationContext* opCtx,
                                    const CollectionPtr& collection,
                                    const BSONObj& indexSpec) const {
    auto normalized = collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpec);
    if (!normalized.isOK()) {
        return normalized.getStatus();
    }
    return prepareSpecForCreate(opCtx, collection, normalized.getValue(), boost::none).getStatus();
}
}  // namespace mongo
