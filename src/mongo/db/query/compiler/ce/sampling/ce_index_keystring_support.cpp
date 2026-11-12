// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/ce_index_keystring_support.h"

#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/assert_util.h"

namespace mongo::ce {

bool requiresKeystringEstimation(const IndexEntry* indexEntry) {
    if (!indexEntry) {
        return false;
    }
    if (internalQuerySamplingCEForceKeystringEstimation.load()) {
        return true;
    }
    return indexEntry->collator != nullptr || indexEntry->type == IndexType::INDEX_HASHED ||
        indexEntry->type == IndexType::INDEX_WILDCARD;
}

namespace {

const IndexCatalogEntry* resolveCatalogEntry(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const IndexEntry& indexEntry) {
    const auto* entry = collection->getIndexCatalog()->findIndexByIdent(
        opCtx, indexEntry.indexCatalogEntryStorage->getIdent());
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream{} << "query plan killed :: index '" << indexEntry.identifier.catalogName
                          << "' dropped",
            entry);
    return entry;
}

const SortedDataIndexAccessMethod* resolveAccessMethod(const IndexCatalogEntry* entry) {
    const auto* accessMethod = entry->accessMethod()->asSortedData();
    tassert(13463701,
            "Expected a SortedDataIndexAccessMethod for index keystring estimation",
            accessMethod != nullptr);
    return accessMethod;
}

}  // namespace

IndexKeystringSampler::IndexKeystringSampler(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const IndexEntry& indexEntry)
    : _opCtx(opCtx),
      _collection(collection),
      _entry(resolveCatalogEntry(opCtx, collection, indexEntry)),
      _accessMethod(resolveAccessMethod(_entry)),
      _keyPattern(indexEntry.keyPattern.getOwned()),
      _ordering(_accessMethod->getSortedDataInterface()->getOrdering()),
      _maxKeystringsPerDocument(
          QueryKnobConfiguration::get(opCtx).getSamplingCEMaxKeystringsPerDocument()),
      _pooledBufferBuilder(key_string::HeapBuilder::kHeapAllocatorDefaultBytes) {}

bool IndexKeystringSampler::decodedKeyMatchesBounds(
    const BSONObj& decodedKey, std::span<const OrderedIntervalList> oils) const {
    auto oilIt = oils.begin();
    auto kpIt = _keyPattern.begin();
    for (auto&& elt : decodedKey) {
        if (oilIt == oils.end()) {
            return true;  // Remaining key components are unconstrained.
        }
        // The interval lists and the key pattern are both in key-pattern order, so a single
        // parallel walk aligns them: skip components without an interval list, and constrain
        // the ones which have one.
        if (kpIt->fieldNameStringData() == oilIt->name) {
            if (!matchesInterval(*oilIt, oilIt->computeDirection(), elt)) {
                return false;
            }
            ++oilIt;
        }
        ++kpIt;
    }
    // Every interval list must have been matched against a key component: the planner emits
    // bounds aligned with the key pattern (for wildcard indexes, in the rewritten
    // {..., $_path: 1, 'path': 1, ...} form).
    tassert(13463703,
            str::stream{} << "Index bounds field is not part of the key pattern: " << oilIt->name,
            oilIt == oils.end());
    return true;
}

bool IndexKeystringSampler::docMatchesBounds(const BSONObj& doc, const IndexBounds& bounds) {
    bool matched = false;
    forEachKey(doc, [&](const BSONObj& decoded) {
        if (decodedKeyMatchesBounds(decoded, bounds.fields)) {
            matched = true;
            return false;  // Stop at the first matching key.
        }
        return true;
    });
    return matched;
}

KeyCountResult IndexKeystringSampler::countNDV(
    const std::vector<FieldPathAndEqSemantics>& fields,
    const std::vector<BSONObj>& docs,
    boost::optional<std::span<const OrderedIntervalList>> bounds) {
    // The keystring path follows the index's own key equality semantics, which unify null and
    // missing (regular equality); strict ($expr) equality is not representable on index keys.
    tassert(13463704,
            "The keystring NDV path does not support strict ($expr) equality semantics",
            std::ranges::none_of(fields, [](const auto& field) { return field.isExprEq; }));

    KeyCountResult counts{};

    std::set<std::vector<BSONElement>, SameSizeVectorBSONElementCmp> distinctKeys;
    std::set<std::vector<BSONElement>, SameSizeVectorBSONElementCmp> docKeys;
    std::vector<BSONObj> distinctKeysOwned;
    std::vector<BSONObj> docKeysOwned;
    for (const auto& doc : docs) {
        docKeys.clear();
        docKeysOwned.clear();
        forEachKey(doc, [&](const BSONObj& decoded) {
            // The decoded BSONObjs reuse a decode buffer, so owned-copy them to
            // keep the stored tuple elements stable.
            const BSONObj stable = decoded.getOwned();
            std::vector<BSONElement> tuple;
            tuple.reserve(fields.size());
            BSONObjIterator stableIt(stable);
            BSONObjIterator keyPatternIt(_keyPattern);
            auto fieldsIt = fields.begin();
            // This assumes fields follows the same order as _keyPattern
            while (fieldsIt < fields.end()) {
                tassert(13463702,
                        str::stream{} << "Field not in key pattern: " << fieldsIt->path,
                        keyPatternIt.more() && stableIt.more());
                if (fieldsIt->path == keyPatternIt.next().fieldNameStringData()) {
                    tuple.push_back(stableIt.next());
                    fieldsIt++;
                } else {
                    stableIt.next();
                }
            }
            const bool isNovelDocKey = docKeys.insert(tuple).second;
            if (!isNovelDocKey) {
                return true;
            }

            docKeysOwned.push_back(stable);
            counts.totalSampleKeys++;

            if (!bounds || decodedKeyMatchesBounds(decoded, *bounds)) {
                counts.uniqueMatchingKeys++;
                const bool isNovelDistinctKey = distinctKeys.insert(std::move(tuple)).second;
                if (isNovelDistinctKey) {
                    distinctKeysOwned.push_back(stable);
                }
            }

            return true;
        });
    }

    counts.sampleUniqueKeys = distinctKeys.size();
    return counts;
}

}  // namespace mongo::ce
