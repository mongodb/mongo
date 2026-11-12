// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/storage/key_string/key_string.h"

#include <span>

#include <boost/container/flat_set.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class CollectionPtr;
}  // namespace mongo

namespace mongo::ce {

/**
 * Returns true when the key materialization of 'indexEntry' cannot be reproduced by projecting
 * field values out of the sample documents, which is what the default samplingCE path does.
 * This is the case for hashed indexes (keys are hashes of the indexed values), wildcard indexes
 * (keys are path/value pairs spread across every field of the document) and indexes with a
 * non-simple collator (key ordering follows the collator, not binary comparison). Such indexes
 * are estimated by generating the actual index keystrings instead.
 * Returns true even for indices where keystring extraction is not needed if the knob
 * internalQuerySamplingCEForceKeystringEstimation is enabled.
 * Always returns false if indexEntry is null.
 */
bool requiresKeystringEstimation(const IndexEntry* indexEntry);

/**
 * Thrown when the index's key generation fails over a sampled document
 * (e.g. a stale persisted sample holding a document the index cannot index, like array values
 * under a hashed index), or when a document generates more than the configured maximum number
 * of keystrings. SamplingEstimatorImpl also throws this for subsequent extraction requests for
 * the same index during the query.
 */
class KeystringExtractionException final : public DBException {
public:
    explicit KeystringExtractionException(std::string reason)
        : DBException(Status(ErrorCodes::CannotBuildIndexKeys, std::move(reason))) {}

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};

/**
 * Generates index keystrings for sample documents and evaluates them against index bounds, for
 * index types that 'requiresKeystringEstimation()' selects. This mirrors what an index scan does
 * at document insertion/query execution time: keys are produced by the index's own key generator
 * (via SortedDataIndexAccessMethod::getKeys()), so hashing, collation and wildcard path handling
 * are exactly the index's own, and bounds -- which the planner emits in index space -- are compared
 * against the decoded keys without any further translation.
 */
class IndexKeystringSampler {
public:
    IndexKeystringSampler(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const IndexEntry& indexEntry);

    IndexKeystringSampler(const IndexKeystringSampler&) = delete;
    IndexKeystringSampler& operator=(const IndexKeystringSampler&) = delete;

    /**
     * Invokes 'callback' once per index key generated for 'doc', in index order.
     * The callback is called with the BSON decoding of the generated keystring with one
     * element per key-pattern component in order, without RecordId.
     * The callback returns true to continue iterating over the remaining keys of the document.
     * The decoded object is only valid until the next call into the sampler as internal buffers
     * are reused across calls.
     * Throws KeystringExtractionException before invoking 'callback' if key generation fails or
     * the document generates more than 'internalQuerySamplingCEMaxKeystringsPerDocument'
     * keystrings.
     */
    template <typename T>
    requires std::invocable<T, const BSONObj&> &&
        std::convertible_to<std::invoke_result_t<T, const BSONObj&>, bool>
    void forEachKey(const BSONObj& doc, const T& callback) {
        _keys.clear();
        _pooledBufferBuilder.freeUnused();
        try {
            _accessMethod->getKeys(
                _opCtx,
                _collection,
                _entry,
                _pooledBufferBuilder,
                doc,
                InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                SortedDataIndexAccessMethod::GetKeysContext::kValidatingKeys,
                &_keys,
                // Sampling does not consume multikey metadata.
                /*multikeyMetadataKeys=*/nullptr,
                /*multikeyPaths=*/nullptr,
                // Do not append RecordId to the generated keystrings.
                /*id=*/boost::none);
        } catch (const DBException& ex) {
            throw KeystringExtractionException(str::stream{} << "Index key extraction failed: "
                                                             << ex.toStatus().reason());
        }
        if (_keys.size() > _maxKeystringsPerDocument) {
            throw KeystringExtractionException(str::stream{}
                                               << "Sample document generated " << _keys.size()
                                               << " keystrings, exceeding the samplingCE limit of "
                                               << _maxKeystringsPerDocument);
        }
        for (const auto& ks : _keys) {
            // Decode the key into its BSON form to match it against the bounds: the interval
            // lists are unions of ranges over the individual key components, so matching is
            // element-wise.
            _decodeBuilder.resetToEmpty();
            key_string::toBsonSafe(ks.getView(), _ordering, ks.getTypeBits(), _decodeBuilder);
            if (!callback(_decodeBuilder.asTempObj())) {
                break;
            }
        }
    }

    /**
     * Keystring counterpart of SamplingEstimatorImpl::forNumberKeysMatch.
     */
    template <typename T>
    requires std::invocable<T, size_t>
    void forNumberKeysMatch(const IndexBounds& bounds,
                            const std::vector<BSONObj>& sample,
                            const T& callback) {
        for (const auto& doc : sample) {
            size_t matchCount = 0;
            forEachKey(doc, [&](const BSONObj& decoded) {
                if (decodedKeyMatchesBounds(decoded, bounds.fields)) {
                    ++matchCount;
                }
                return true;
            });
            callback(matchCount);
        }
    }

    /**
     * Keystring counterpart of SamplingEstimatorImpl::forDocumentsMatchingBounds.
     */
    template <typename T>
    requires std::is_invocable_r_v<bool, T, const BSONObj&>
    void forDocumentsMatchingBounds(const IndexBounds& bounds,
                                    const std::vector<BSONObj>& docs,
                                    const T& callback) {
        for (const auto& doc : docs) {
            if (docMatchesBounds(doc, bounds)) {
                if (!callback(doc)) {
                    return;
                }
            }
        }
    }

    /**
     * Keystring counterpart of countNDV().
     */
    KeyCountResult countNDV(
        const std::vector<FieldPathAndEqSemantics>& fields,
        const std::vector<BSONObj>& docs,
        boost::optional<std::span<const OrderedIntervalList>> bounds = boost::none);

private:
    /**
     * Returns true if any index key generated for 'doc' falls within 'bounds'.
     */
    bool docMatchesBounds(const BSONObj& doc, const IndexBounds& bounds);

    /**
     * Returns true if the decoded index key matches the ordered interval lists. The interval
     * lists and the key pattern are walked in parallel -- both are in key-pattern order, and
     * the interval lists may skip components. Components without an interval list are
     * unconstrained.
     */
    bool decodedKeyMatchesBounds(const BSONObj& decodedKey,
                                 std::span<const OrderedIntervalList> oils) const;

    OperationContext* _opCtx;
    const CollectionPtr& _collection;
    const IndexCatalogEntry* _entry;
    const SortedDataIndexAccessMethod* _accessMethod;
    BSONObj _keyPattern;
    Ordering _ordering;
    const size_t _maxKeystringsPerDocument;
    SharedBufferFragmentBuilder _pooledBufferBuilder;
    KeyStringSet _keys;
    BSONObjBuilder _decodeBuilder;
};

}  // namespace mongo::ce
